#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#ifdef ANDROID
#include <android/log.h>
#endif
#include <jni_sub.h>
#include <errno.h>
#include <poll.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>

#include "main.h"
#include "alsa_priv.h"


#define MAX_POLL_WAIT_MS	(10*1000)	

static ssize_t write_offload(playback_ctx *ctx, void *buf, size_t count)
{
    struct pollfd fds;
    struct snd_compr_avail avail;
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
    int ret, written = 0;

    while(written != count) {		
	while(1) {
	    if(ioctl(priv->fd, SNDRV_COMPRESS_AVAIL, &avail) != 0) {
		log_err("SNDRV_COMPRESS_AVAIL failed, exiting");
		return -1;
	    }
	    if(avail.avail >= count) break;
	    fds.fd = priv->fd;
	    fds.events = POLLOUT;
	    ret = poll(&fds, 1, MAX_POLL_WAIT_MS);
	    if(ret == 0 || (ret < 0 && errno == EBADFD)) return 0;	/* we're paused */
	    if(ret < 0 || (fds.revents & POLLERR)) {
		log_err("poll returned error: %s", strerror(errno));
		return -1;
	    }
	}
	ret = write(priv->fd, buf, count - written);
	if(ret < 0) {
	    if(errno == EBADFD) return 0;				/* we're paused */
	    return ret;	
	}
	written += ret;
	buf += ret;
    }
    return written;	
}

/* fd = opened source file descriptor, start_offset points to data after 
   compressed file header if any.
   ctx is assumed to contain all required file header data.
 */

#define MMAP_SIZE       (128*1024*1024)

int alsa_play_offload(playback_ctx *ctx, int fd, off_t start_offset)
{
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
    int k, ret = 0;
    char tmp[128];	
    struct snd_compr_caps caps;
    struct snd_compr_params params;
    off_t flen = 0;
    off_t off, cur_map_off;     /* file offset currently mapped to mm */
    size_t cur_map_len;         /* size of file chunk currently mapped */
    const off_t pg_mask = sysconf(_SC_PAGESIZE) - 1;
    void *mptr, *mend, *mm = MAP_FAILED;

	pthread_mutex_lock(&ctx->mutex);
	if(ctx->state != STATE_STOPPED) {
	    log_info("context live, stopping");
	    pthread_mutex_unlock(&ctx->mutex);  
	    audio_stop(ctx,1); 
	    pthread_mutex_lock(&ctx->mutex);
	    log_info("live context stopped");   
	}

	flen = lseek(fd, 0, SEEK_END);
	if(flen == (off_t) -1) {
	    log_err("source file seek failed");
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto err_exit;	
	}

	lseek(fd, 0, SEEK_SET);	
	cur_map_off = start_offset & ~pg_mask;
	cur_map_len = (flen - cur_map_off) > MMAP_SIZE ? MMAP_SIZE : flen - cur_map_off;

	mm = mmap(0, cur_map_len, PROT_READ, MAP_SHARED, fd, cur_map_off);
	if(mm == MAP_FAILED) {
	    log_err("mmap failed after seek: %s", strerror(errno));
	    ret = LIBLOSSLESS_ERR_INIT;
	    goto err_exit;
	}

	mptr = mm + (start_offset & pg_mask);
	mend = mm + cur_map_len;

	if(priv->nv_start) set_mixer_controls(ctx, priv->nv_start);
	else log_info("no start controls for this device");

	priv->cur_fmt = 0;
	if(ctx->bps == 24) priv->cur_fmt++;
	alsa_set_volume(ctx, VOL_SET_CURRENT);	/* must reset according to cur_fmt */

	log_info("opening offload device");
	sprintf(tmp, "/dev/snd/comprC%dD%d", priv->card, priv->device);
	priv->fd = open(tmp, O_WRONLY);
	if(priv->fd < 0) {
	    log_err("cannot open %s", tmp);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;
	}
	log_info("offload device opened");
	if(ioctl(priv->fd, SNDRV_COMPRESS_GET_CAPS, &caps) != 0) {
	    log_err("cannot get compress capabilities for device %d", priv->device);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;
	}

	priv->chunks = caps.max_fragments;
	priv->chunk_size = caps.min_fragment_size;
	log_info("device reports: max %d chunks of min %d size", priv->chunks, priv->chunk_size);	

	k = alsa_get_rate(ctx->samplerate);
	if(k < 0) {
	    log_err("unsupported playback rate");
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}

	memset(&params, 0, sizeof(params));
	params.codec.ch_in = ctx->channels;			
	params.codec.ch_out = ctx->channels;
	params.codec.sample_rate = k;

	log_info("flac: chans=%d, bps=%d, rate=%d (%x)\n", ctx->channels, ctx->bps, ctx->samplerate, k);

	switch(ctx->file_format) {
	    case FORMAT_FLAC:	
		params.codec.id = SND_AUDIOCODEC_FLAC;
		params.codec.options.flac_dec.sample_size = ctx->bps;
		params.codec.options.flac_dec.min_blk_size = ctx->block_min;
		params.codec.options.flac_dec.max_blk_size = ctx->block_max;
		params.codec.options.flac_dec.min_frame_size = ctx->frame_min;
		params.codec.options.flac_dec.max_frame_size = ctx->frame_max;
		/* Fuck. This field was intended for SND_AUDIOSTREAMFORMAT macros
		   in compress_params.h, e.g. SND_AUDIOSTREAMFORMAT_(FLAC|FLAC_OGG), etc. */
		params.codec.format = ctx->bps == 24 ? SNDRV_PCM_FORMAT_S24_LE : SNDRV_PCM_FORMAT_S16_LE;
		params.codec.profile = SND_AUDIOPROFILE_FLAC;
		break;
	    case FORMAT_MP3:
		params.codec.id = SND_AUDIOCODEC_MP3;
		params.codec.bit_rate = ctx->bitrate;
		break;
	    default:
		log_err("unsupported playback format %d", ctx->file_format);
		ret = LIBLOSSLESS_ERR_AU_SETUP;
		goto err_exit;	
	}

	params.buffer.fragments = priv->chunks; 
	params.buffer.fragment_size = priv->chunk_size;

        log_info("setting params: codec=%d block_sz=%d/%d frm_sz=%d/%d smpl_sz=%d, %d chunks of size %d\n", 
		params.codec.id, params.codec.options.flac_dec.min_blk_size,
                params.codec.options.flac_dec.max_blk_size, params.codec.options.flac_dec.min_frame_size, 
		params.codec.options.flac_dec.max_frame_size, params.codec.options.flac_dec.sample_size,
		priv->chunks, priv->chunk_size);

	while(ioctl(priv->fd, SNDRV_COMPRESS_SET_PARAMS, &params) != 0) {
	    priv->chunks >>= 1;
	    if(!priv->chunks) {
		log_err("failed to set hardware parameters");
		ret = LIBLOSSLESS_ERR_AU_SETUP;
		goto err_exit;
	    }				
	    params.buffer.fragments = priv->chunks;
	    log_info("hw params setup failed, testing with %d chunks", priv->chunks); 
	}

	log_info("offload playback setup succeeded");

	/* Write full buffer initially, then start playback and continue writing  */
	if(mend - mptr < priv->chunks * priv->chunk_size) {
	    log_err("I'm offended, won't bother playing %d bytes.", (int) (mend - mptr));
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto err_exit;	
	}
	k = priv->chunks * priv->chunk_size;
	ret = write_offload(ctx, mptr, k);
	if(ret != k) {
	    log_err("error writing initial chunks: ret=%d", ret);
	    ret = LIBLOSSLESS_ERR_IO_WRITE;
	    goto err_exit;	
	}
	mptr += k;
	log_info("starting playback");
	if(ioctl(priv->fd, SNDRV_COMPRESS_START) != 0) {
	    log_err("failed to start playback");
	    ret = LIBLOSSLESS_ERR_AU_START;
	    goto err_exit;
	}
	log_info("playback started");
	ctx->state = STATE_PLAYING;
        ctx->stopped = 0;
        ctx->alsa_error = 0;
	ctx->audio_thread = 0;
	pthread_mutex_unlock(&ctx->mutex);

	while(mptr < mend) {
	    k = sync_state(ctx, __func__);		/* will block while we're paused */
	    if(k < 0)  {
		log_info("gather I should stop");	/* or return negative if we're stopped. */
		break;				
	    }	
	    k = (mend - mptr < priv->chunk_size) ? mend - mptr : priv->chunk_size;
	    if(k < priv->chunk_size && cur_map_off + cur_map_len != flen) {        /* too close to end of mapped region, but not at eof */
		log_info("remapping");  
		munmap(mm, cur_map_len);
		off = mptr - mm;
		cur_map_off = (cur_map_off + off) & ~pg_mask;
		cur_map_len = (flen - cur_map_off) > MMAP_SIZE ? MMAP_SIZE : flen - cur_map_off;
		mm = mmap(0, cur_map_len, PROT_READ, MAP_SHARED, fd, cur_map_off);
		if(mm == MAP_FAILED) {
		    log_err("mmap failed after seek: %s", strerror(errno));     
		    ret = LIBLOSSLESS_ERR_IO_READ;
		    goto err_exit_unlocked;  
		}       
		mptr = mm + (off & pg_mask);
		mend = mm + cur_map_len;
		k = (mend - mptr < priv->chunk_size) ? mend - mptr : priv->chunk_size;        
		log_info("remapped");
	    }
	    ret = write_offload(ctx, mptr, k);	
	    if(ret == 0) continue;	/* we're in PAUSE state; should block in sync_state() now. */
	    if(ret != k) {
		log_err("write error: ret=%d, errno=%d %s", ret, errno, strerror(errno));
		ret = LIBLOSSLESS_ERR_IO_WRITE;
		goto err_exit_unlocked;
	    }	
	    mptr += k;
	}
	if(mptr == mend) {
	    log_info("draining");
	    if(ioctl(priv->fd, SNDRV_COMPRESS_DRAIN) != 0) log_info("drain failed");
	}
	log_info("exiting");
	close(fd);
	audio_stop(ctx, 0);
	munmap(mm, cur_map_len);
	return 0;

    err_exit:
	pthread_mutex_unlock(&ctx->mutex);
    err_exit_unlocked:
	close(fd);
	if(mm != MAP_FAILED) munmap(mm, cur_map_len);
	if(ctx->state != STATE_STOPPED) audio_stop(ctx, 0);
    return ret;	
} 


bool alsa_pause_offload(playback_ctx *ctx) 
{
    alsa_priv *priv;
	if(!ctx || !ctx->alsa_priv) return false;
	priv = (alsa_priv *) ctx->alsa_priv;
    return ioctl(priv->fd, SNDRV_COMPRESS_PAUSE, 1) == 0;
}

bool alsa_resume_offload(playback_ctx *ctx) 
{
    alsa_priv *priv;
	if(!ctx || !ctx->alsa_priv) return false;
	priv = (alsa_priv *) ctx->alsa_priv;
    return ioctl(priv->fd, SNDRV_COMPRESS_RESUME, 0) == 0;
}

int alsa_time_pos_offload(playback_ctx *ctx)
{
    alsa_priv *priv;
    struct snd_compr_avail avail;
	if(!ctx || !ctx->alsa_priv) return 0;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(ioctl(priv->fd, SNDRV_COMPRESS_AVAIL, &avail) != 0) return 0;
	if(avail.tstamp.sampling_rate == 0) return 0;
    return avail.tstamp.pcm_io_frames / avail.tstamp.sampling_rate;
}


