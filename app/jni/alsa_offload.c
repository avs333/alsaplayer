#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
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
#include "main.h"
#include "alsa_priv.h"

#undef _COMPR_PROTO_
#include "compr.h"

#define MAX_POLL_WAIT_MS	(10*1000)

static ssize_t write_offload(playback_ctx *ctx, void *buf, size_t count)
{
    struct pollfd fds;
    int avail;
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
    int ret, written = 0;
    while(written != count) {		
	while(1) {
	    if((*compr_avail)(priv->fd, &avail) != 0) {
		log_err("SNDRV_COMPRESS_AVAIL failed, exiting");
		return -1;
	    }
	    if(avail >= count) break;
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

static void convert24_3le_le(void *src, void *dst, int src_bytes)
{
    int k;	
    int8_t *s = (int8_t *) src;
    int8_t *d = (int8_t *) dst;
	for(k = 0; k < src_bytes/3; k++) {
#if 0
	    d[0] = s[0];	
	    d[1] = s[1];	
	    d[2] = s[2];	
	    d[3] = (s[2] < 0) ? 0xff : 0;
#else		/* wtf??? */
	    d[0] = 0;
	    d[1] = s[0];	
	    d[2] = s[1];	
	    d[3] = s[2];	
#endif
	    s += 3; d += 4;
	}
} 

/* fd = opened source file descriptor, start_offset points to data after 
   compressed file header if any.
   ctx is assumed to contain all required file header data.
 */

#define MMAP_SIZE       (128*1024*1024)

int alsa_play_offload(playback_ctx *ctx, int fd, off_t start_offset)
{
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
    int k, read_size, write_size, ret = 0;
    char tmp[128];	
    off_t flen = 0;
    off_t off, cur_map_off;     /* file offset currently mapped to mm */
    size_t cur_map_len;         /* size of file chunk currently mapped */
    const off_t pg_mask = sysconf(_SC_PAGESIZE) - 1;
    void *mptr, *mend, *mm = MAP_FAILED;
    void *write_buff = 0;
    struct timeval tstart, tstop, tdiff;
    int min_fragments, max_fragments, min_fragment_size, max_fragment_size;
 	
	pthread_mutex_lock(&ctx->mutex);
	if(ctx->state != STATE_STOPPED) {
	    log_info("context live, stopping");
	    pthread_mutex_unlock(&ctx->mutex);  
	    audio_stop(ctx); 
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

	log_info("opening offload playback device");
	sprintf(tmp, "/dev/snd/comprC%dD%d", priv->card, priv->device);
	priv->fd = open(tmp, O_WRONLY);
	if(priv->fd < 0) {
	    log_err("cannot open %s", tmp);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;
	}
	log_info("offload playback device opened");

	if((*compr_get_caps)(priv->fd, &min_fragments, 
		&max_fragments, &min_fragment_size, &max_fragment_size) != 0) {
	    log_err("cannot get compress capabilities for device %d", priv->device);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;
	}

	log_info("device reports: max %d chunks of min %d size", max_fragments, min_fragment_size);	
#ifndef ANDROID
	priv->chunks = forced_chunks ? forced_chunks : max_fragments;
	priv->chunk_size = forced_chunk_size ? forced_chunk_size : min_fragment_size;
	k = (*compr_set_hw_params)(ctx, priv->fd, priv->chunks, priv->chunk_size, forced_chunks || forced_chunk_size);
#else
	priv->chunks = max_fragments;
	priv->chunk_size = min_fragment_size;
	k = (*compr_set_hw_params)(ctx, priv->fd, priv->chunks, priv->chunk_size, 0);
#endif
	if(k != 0) {
	    ret = k;
	    goto err_exit;
	}

	log_info("offload playback setup succeeded");

	if(ctx->file_format == FORMAT_WAV && ctx->bps == 24) {
	    if(priv->chunk_size & 3) {
		log_err("chunk size not divisible by 4 for 24-bit stream");
		ret = LIBLOSSLESS_ERR_AU_SETUP;
		goto err_exit;
	    } 	
	    write_buff = (int8_t *) malloc(priv->chunks * priv->chunk_size);
	    if(!write_buff) {
		log_err("no memory");
		ret = LIBLOSSLESS_ERR_NOMEM;
		goto err_exit;
	    }		
	    read_size = 3 * (priv->chunk_size/4);
	} else {
	    write_buff = 0;	
	    read_size = priv->chunk_size;
	}

	/* Write full buffer initially, then start playback and continue writing  */
	if(mend - mptr < priv->chunks * read_size) {
	    log_err("I'm offended, won't bother playing %d bytes.", (int) (mend - mptr));
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto err_exit;	
	}

	gettimeofday(&tstart,0);

	k = priv->chunks * priv->chunk_size;
	if(write_buff) {
	    convert24_3le_le(mptr, write_buff, priv->chunks * read_size);
	    ret = write_offload(ctx, write_buff, k);
	} else ret = write_offload(ctx, mptr, k);

	if(ret != k) {
	    log_err("error writing initial chunks: ret=%d", ret);
	    ret = LIBLOSSLESS_ERR_IO_WRITE;
	    goto err_exit;	
	}
	mptr += priv->chunks * read_size;

	if(write_buff) {	/* free extra memory */
	    write_buff = (int8_t *) realloc(write_buff, priv->chunk_size);
	    if(!write_buff) {
		log_err("no memory");
		ret = LIBLOSSLESS_ERR_NOMEM;
		goto err_exit;
	    }		
	}

	log_info("starting playback");
	if((*compr_start_playback)(priv->fd) != 0) {
	    log_err("failed to start playback");
	    ret = LIBLOSSLESS_ERR_AU_START;
	    goto err_exit;
	}
	log_info("playback started");
	ctx->state = STATE_PLAYING;
        ctx->alsa_error = 0;
	ctx->audio_thread = 0;
	pthread_mutex_unlock(&ctx->mutex);

	while(mptr < mend) {
	    k = (mend - mptr < read_size) ? mend - mptr : read_size;
	    if(k < read_size && cur_map_off + cur_map_len != flen) {        /* too close to end of mapped region, but not at eof */
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
		k = (mend - mptr < read_size) ? mend - mptr : read_size;        
		log_info("remapped");
	    }
	    if(sync_state(ctx, __func__) != STATE_PLAYING)  {
		log_info("gather I should stop");
		break;				
	    }	
	    if(write_buff) {
		convert24_3le_le(mptr, write_buff, k);
		write_size = (4 * k)/3;
		ret = write_offload(ctx, write_buff, write_size);
	    } else {
		write_size = k;
		ret = write_offload(ctx, mptr, k);
	    }		
	    if(ret == 0) continue;	/* we're in PAUSE state; should block in sync_state() now. */
	    if(ret != write_size) {
		log_err("write error: ret=%d, errno=%d %s", ret, errno, strerror(errno));
		ret = LIBLOSSLESS_ERR_IO_WRITE;
		goto err_exit_unlocked;
	    }	
	    mptr += k;
	}

	if(mptr == mend) {
	    log_info("draining");
	    if((*compr_drain)(priv->fd) != 0) log_info("drain failed");
	}

	gettimeofday(&tstop,0);
	timersub(&tstop, &tstart, &tdiff);
	log_info("playback time %ld.%03ld sec", tdiff.tv_sec, tdiff.tv_usec/1000);	
	close(fd);
	munmap(mm, cur_map_len);
	if(write_buff) free(write_buff);
	playback_complete(ctx, __func__);
	return 0;

    err_exit:
	pthread_mutex_unlock(&ctx->mutex);
    err_exit_unlocked:
	close(fd);
	if(mm != MAP_FAILED) munmap(mm, cur_map_len);
	if(write_buff) free(write_buff);
	playback_complete(ctx, __func__);
    return ret;	
} 


bool alsa_pause_offload(playback_ctx *ctx) 
{
    alsa_priv *priv;
	if(!ctx || !ctx->alsa_priv) return false;
	priv = (alsa_priv *) ctx->alsa_priv;
    return (*compr_pause)(priv->fd) == 0;
}

bool alsa_resume_offload(playback_ctx *ctx) 
{
    alsa_priv *priv;
	if(!ctx || !ctx->alsa_priv) return false;
	priv = (alsa_priv *) ctx->alsa_priv;
    return (*compr_resume)(priv->fd) == 0;
}

int alsa_time_pos_offload(playback_ctx *ctx)
{
    alsa_priv *priv;
	if(!ctx || !ctx->alsa_priv) return 0;
	priv = (alsa_priv *) ctx->alsa_priv;
    return (*compr_offload_time_pos)(priv->fd);
}

/* ************************************************************* */
/* MP3 playback: taken verbatim from tinycompress library sample */
/* ************************************************************* */

#define MP3_SYNC 0xe0ff

const int mp3_sample_rates[3][3] = {
	{44100, 48000, 32000},        /* MPEG-1 */
	{22050, 24000, 16000},        /* MPEG-2 */
	{11025, 12000,  8000},        /* MPEG-2.5 */
};

const int mp3_bit_rates[3][3][15] = {
	{
		/* MPEG-1 */
		{  0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, /* Layer 1 */
		{  0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384}, /* Layer 2 */
		{  0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}, /* Layer 3 */
	},
	{
		/* MPEG-2 */
		{  0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, /* Layer 1 */
		{  0,  8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, /* Layer 2 */
		{  0,  8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, /* Layer 3 */
	},
	{
		/* MPEG-2.5 */
		{  0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, /* Layer 1 */
		{  0,  8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, /* Layer 2 */
		{  0,  8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, /* Layer 3 */
	},
};

enum mpeg_version {
	MPEG1  = 0,
	MPEG2  = 1,
	MPEG25 = 2
};

enum mp3_stereo_mode {
	STEREO = 0x00,
	JOINT = 0x01,
	DUAL = 0x02,
	MONO = 0x03
};

struct mp3_header {
	uint16_t sync;
	uint8_t format1;
	uint8_t format2;
};

int parse_mp3_header(struct mp3_header *header, int *num_channels,
		int *sample_rate, int *bit_rate)
{
	int ver_idx, mp3_version, layer, bit_rate_idx, sample_rate_idx, channel_idx;

	/* check sync bits */
	if ((header->sync & MP3_SYNC) != MP3_SYNC) {
		fprintf(stderr, "Error: Can't find sync word\n");
		return -1;
	}
	ver_idx = (header->sync >> 11) & 0x03;
	mp3_version = ver_idx == 0 ? MPEG25 : ((ver_idx & 0x1) ? MPEG1 : MPEG2);
	layer = 4 - ((header->sync >> 9) & 0x03);
	bit_rate_idx = ((header->format1 >> 4) & 0x0f);
	sample_rate_idx = ((header->format1 >> 2) & 0x03);
	channel_idx = ((header->format2 >> 6) & 0x03);

	if (sample_rate_idx == 3 || layer == 4 || bit_rate_idx == 15) {
		log_err("Error: Can't find valid header");
		return -1;
	}
	*num_channels = (channel_idx == MONO ? 1 : 2);
	*sample_rate = mp3_sample_rates[mp3_version][sample_rate_idx];
	*bit_rate = (mp3_bit_rates[mp3_version][layer - 1][bit_rate_idx]) * 1000;
	return 0;
}

int mp3_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start) 
{
    int ret, fd = -1;	
    const char *file = 0;
    off_t flen = 0, offs = 0;
    struct mp3_header hdr;
    unsigned char tmp[16];
#ifdef ANDROID
	file = (*env)->GetStringUTFChars(env,jfile,NULL);
	if(!file) {
	    log_err("no file specified");
	    ret = LIBLOSSLESS_ERR_INV_PARM;
	    goto done;
	}
	log_info("attempting to play %s at offset %d", file, start);
#else
	file = jfile;
#endif
	fd = open(file, O_RDONLY);
	if(fd < 0) {
	    log_err("cannot open %s", file);
	    ret = LIBLOSSLESS_ERR_NOFILE;
            goto done;
	}
	flen = lseek(fd, 0, SEEK_END);
	if(flen < 0) {
	    log_err("lseek failed for %s", file);
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto done;  
	}
	lseek(fd, 0, SEEK_SET);
	if(read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
	    log_err("file %s unreadable", file);	
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto done;  
	}	
	if(memcmp(&hdr, "ID3", 3) == 0) {
	    lseek(fd, 0, SEEK_SET);
	    if(read(fd, &tmp, 10) != 10) {
		log_err("read error for file %s", file);	
		ret = LIBLOSSLESS_ERR_IO_READ;
		goto done;  
	    }	
	    offs = tmp[6] << 21;
	    offs += tmp[7] << 14;
	    offs += tmp[8] << 7;
	    offs += tmp[9];
	    if(lseek(fd, offs, SEEK_CUR) == (off_t) -1) {
		log_err("broken ID3 header in %s", file);
		ret = LIBLOSSLESS_ERR_DECODE;
		goto done;
	    }
	    if(read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		log_err("file %s unreadable", file);	
		ret = LIBLOSSLESS_ERR_IO_READ;
		goto done;  
	    }	
	}
	if(parse_mp3_header(&hdr, &ctx->channels, &ctx->samplerate, &ctx->bitrate) == -1) {
	    log_err("error parsing mp3 headers for %s", file);
	    ret = LIBLOSSLESS_ERR_NOFILE;
	    goto done;  
	}
	if(ctx->bitrate) {	/* crap. */
	    ctx->track_time = (flen * 8)/ctx->bitrate;
	    if(start >= ctx->track_time) {	
		ret = LIBLOSSLESS_ERR_OFFSET;
		goto done;
	    }	
	    offs = (start * flen)/ctx->track_time;
	} else offs = 0;
	update_track_time(env, obj, ctx->track_time);
	log_info("switching to offload playback");
	ret = alsa_play_offload(ctx, fd, offs);
    done:
#ifdef ANDROID
	if(file) (*env)->ReleaseStringUTFChars(env,jfile,file);
#endif
	if(fd >= 0) close(fd);
	return ret;
}





