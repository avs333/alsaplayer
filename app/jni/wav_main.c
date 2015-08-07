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
#include <limits.h>
#include <sys/time.h>
#ifdef ANDROID
#include <android/log.h>
#endif
#include <jni_sub.h>

#include <errno.h>
#include <signal.h>

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

#include "main.h"


#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

#define WAVE_FORMAT_PCM		1
#define WAVE_FORMAT_EXTENSIBLE	0xFFFE

struct riff_wave_header {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t wave_id;
};

struct chunk_header {
    uint32_t id;
    uint32_t sz;
};

struct chunk_fmt {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

struct ext_fmt {
    uint16_t ext_size;
    uint16_t num_vaild_bits;
    uint32_t speaker_mask;
    uint8_t guid[16];		
};

static const uint8_t pcm_guid[16] = {
    1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71
};

static off_t wav_init(int *samplerate, int *channels, int *bps, void *mm, int len)
{
    struct riff_wave_header *rh;
    struct chunk_fmt *fmt; 	
    int more_chunks = 1;
    int format_found = 0;
    void *mptr = mm, *mend = mm + len;
    
    rh = (struct riff_wave_header *) mptr;
    mptr += sizeof(struct riff_wave_header);	
    if(mptr > mend) {
	log_err("eof while reading riff header");
	return 0;
    }		    
    if (rh->riff_id != ID_RIFF || rh->wave_id != ID_WAVE) {
        log_err("Error: not a riff/wave file");
        return 0;
    }
    do {
    	struct chunk_header *chh = (struct chunk_header *) mptr;
	mptr += sizeof(struct chunk_header);	
	if(mptr > mend) {
	    log_err("eof while reading chunk header");
	    return 0;
	}		    
        switch (chh->id) {
	    case ID_FMT:
		fmt = (struct chunk_fmt *) mptr;
		mptr += sizeof(struct chunk_fmt);
		if(fmt->audio_format == WAVE_FORMAT_PCM || 
			fmt->audio_format == WAVE_FORMAT_EXTENSIBLE) {
		    *samplerate = fmt->sample_rate;
		    *channels = fmt->num_channels;
		    *bps = fmt->bits_per_sample;	
		} else {		
 		    log_err("unsupported audio format of wave file");
		    return 0;
		}
		if(fmt->audio_format == WAVE_FORMAT_EXTENSIBLE) {
		    struct ext_fmt *ext = (struct ext_fmt *) mptr;	
		    if(mend - mptr < sizeof(struct ext_fmt)) {
			log_err("eof while reading extended fmt header");
			return 0;
		    }
		    if(memcmp(ext->guid, &pcm_guid, 16) != 0) {
			log_err("unsupported extended audio format");
			return 0;
		    }
		} 
		/* If the format header is larger, skip the rest */
		if (chh->sz > sizeof(struct chunk_fmt))
		    mptr += (chh->sz - sizeof(struct chunk_fmt));
		format_found = 1;
		break;
	    case ID_DATA:
		/* Stop looking for chunks */
		more_chunks = 0;
		break;
	    default:
		/* Unknown chunk, skip bytes */
		mptr += chh->sz;
		break;
        }
	if(mptr > mend) {
	    log_err("short file");
	    return 0;
	}			
    } while(more_chunks);

    if(!format_found) {
        fprintf(stderr, "No format chunk in wav file\n");
        return 0;
    }
    return mptr - mm;
}

#if defined(__ARM_ARCH_7A__)
/* Some 8 times faster than C-version */
void __attribute__((naked)) convert24_asm(int8_t *dst, int8_t *src, int num_24bit_chunks_divided_by4)
{
    asm(".syntax unified\n"
	"push	{r4-r9, lr}\n"
	"mov	r3, 0\n"
    "1:  ldr	r4, [r1], 4\n"		/* we process (24/8)*4 = 12 bytes per cycle */
	"ldr	r5, [r1], 4\n"
	"ldr	r6, [r1], 4\n"
	"mov	r7, r4\n"
	"tst	r4, 0x800000\n"
	"ite	eq\n"			/* zero sign bit */
	"andeq	r7, 0x00ffffff\n"
	"orrne	r7, 0xff000000\n"
	"lsr	r8, r4, 24\n"
	"str	r7, [r0], 4\n"
	"orr	r9, r8, r5, lsl 8\n"
	"tst	r5, 0x8000\n"
	"ite	eq\n"	
	"andeq	r9, 0x00ffffff\n"
	"orrne	r9, 0xff000000\n"
	"lsr	r7, r5, 16\n"
	"str	r9, [r0], 4\n"
	"orr	r8, r7, r6, lsl 16\n"
	"tst	r6, 0x80\n"
	"ite	eq\n"
	"andeq	r8, 0x00ffffff\n"
	"orrne	r8, 0xff000000\n"
	"asr	r7, r6, 8\n"
	"str	r8, [r0], 4\n"
	"str	r7, [r0], 4\n"
	"add	r3, 1\n"
	"cmp	r2, r3\n"
	"bne	1b\n"
	"pop	{r4-r9, pc}\n");
}
#endif

#define MMAP_SIZE       (128*1024*1024)

int wav_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start) 
{
    int i, k, read_bytes, ret = 0, fd = -1;
    int samplerate = 0, channels = 0, bps = 0, b2f;		/* b2f = bytes->frames */
    int own_buf = 0;
    void *mptr, *mend, *mm = MAP_FAILED;
    void *pcmbuf = 0; 
    const char *file = 0;
    off_t flen = 0; 
    off_t off, cur_map_off; /* file offset currently mapped to mm */
    size_t cur_map_len;	
    const off_t pg_mask = sysconf(_SC_PAGESIZE) - 1;    
    const playback_format_t *format;	
    struct timeval tstart, tstop, tdiff;
#if defined(__ARM_ARCH_7A__)
    int optimise = 0;	
#endif

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
	    ret = LIBLOSSLESS_ERR_INIT;
	    goto done;	
	}
	lseek(fd, 0, SEEK_SET);

	cur_map_off = 0;
	cur_map_len = flen > MMAP_SIZE ? MMAP_SIZE : flen;

	mm = mmap(0, cur_map_len, PROT_READ, MAP_SHARED, fd, 0);
	if(mm == MAP_FAILED) {
	    log_err("mmap failed for %s [len=%ld]: %s", file, flen, strerror(errno));	
	    ret = LIBLOSSLESS_ERR_INIT;
	    goto done;	
	}
#ifdef ANDROID
	if(file) (*env)->ReleaseStringUTFChars(env,jfile,file);
#endif
	off = wav_init(&samplerate, &channels, &bps, mm, cur_map_len);

	if(!off || !samplerate || !channels || !bps) {
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;	
	}
	/* needed for seek */

	ctx->samplerate = samplerate;	/* ctx->samplerate may change after audio_start() */
	ctx->channels = channels;	
	ctx->bps = bps;
	ctx->bitrate = samplerate * channels * bps;
	k = (channels * bps)/8;
	ctx->track_time = ((flen - off)/k) / samplerate;

/*
	TODO!!
	ctx->frame_min = wc->min_framesize;
	ctx->frame_max = wc->max_framesize;
	ctx->block_min = wc->min_blocksize;
	ctx->block_max = wc->max_blocksize;
*/


	/* set starting frame */
        if(start) {
	    off += (start * samplerate * channels * bps/8);	
	    if(lseek(fd, off, SEEK_SET) != off) {
		ret = LIBLOSSLESS_ERR_OFFSET;
		log_err("seek to %d sec failed", start);
		goto done; 
	    }
	    munmap(mm, cur_map_len);
	    if(alsa_is_offload(ctx)) {
		log_info("switching to offload playback");
		update_track_time(env, obj, ctx->track_time);
		return alsa_play_offload(ctx,fd,off);
	    }		
	    cur_map_off = off & ~pg_mask;
	    cur_map_len = (flen - cur_map_off) > MMAP_SIZE ? MMAP_SIZE : flen - cur_map_off;
	    mm = mmap(0, cur_map_len, PROT_READ, MAP_SHARED, fd, cur_map_off);
	    if(mm == MAP_FAILED) {
		log_err("mmap failed after seek: %s", strerror(errno));	
		ret = LIBLOSSLESS_ERR_INIT;
		goto done;	
	    }	
	    mptr = mm + (off & pg_mask);
	    mend = mm + cur_map_len;	
	} else {
	    if(alsa_is_offload(ctx)) {
		munmap(mm, cur_map_len);
		log_info("switching to offload playback");
		update_track_time(env, obj, ctx->track_time);
		return alsa_play_offload(ctx,fd,off);
	    }		
	    mptr = mm + off;
	    mend = mm + cur_map_len;
	}

	ret = audio_start(ctx, 0);
	if(ret) goto done;

	if(ctx->rate_dec != 0) {
	    log_err("Samplerate %d not supported by hw: won't resample WAV files!", samplerate);	
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;	
	}

	format = alsa_get_format(ctx);		/* format selected in alsa_start() */

	read_bytes = alsa_get_period_size(ctx) * channels * (bps/8);
	own_buf = alsa_is_mmapped(ctx) && (format->fmt == SNDRV_PCM_FORMAT_S24_LE);

	if(own_buf) {
	    pcmbuf = malloc(read_bytes);
	    if(!pcmbuf) {
		log_err("no memory for pcmbuf");
		ret = LIBLOSSLESS_ERR_NOMEM;
		goto done;
	    }	
	} else pcmbuf = alsa_get_buffer(ctx);


	switch(format->fmt) {
	    case SNDRV_PCM_FORMAT_S16_LE:	
		b2f = channels * 2; 
		break;
	    case SNDRV_PCM_FORMAT_S24_LE:
#if defined(__ARM_ARCH_7A__)
		if(format->fmt == SNDRV_PCM_FORMAT_S24_LE) {
		    optimise = (read_bytes % 12 == 0);
		    log_info("%susing assembly-optimised code for ARM", optimise ? "" : "not ");
		}
#endif
	    case SNDRV_PCM_FORMAT_S24_3LE:	
		b2f = channels * 3; 
		break;
	    case SNDRV_PCM_FORMAT_S32_LE:	
		b2f = channels * 4; 
		break;
	    default: 
		log_err("internal error: format not supported");
		ret = LIBLOSSLESS_ERR_FORMAT;
		goto done;
	}	

    	log_info("Source: %d-bit %d-channel %d Hz time=%d", bps, channels, samplerate, ctx->track_time);

	update_track_time(env, obj, ctx->track_time);
	gettimeofday(&tstart,0);

	while(mptr < mend) {

	    i = sync_state(ctx, __func__);
	    if(i < 0) break;
	
	    i = (mend - mptr < read_bytes) ? mend - mptr : read_bytes;
	    if(i < read_bytes && cur_map_off + cur_map_len != flen) {	/* too close to end of mapped region, but not at eof */
		log_info("remapping");	
		munmap(mm, cur_map_len);
		off = mptr - mm;
		cur_map_off = (cur_map_off + off) & ~pg_mask;
		cur_map_len = (flen - cur_map_off) > MMAP_SIZE ? MMAP_SIZE : flen - cur_map_off;
		mm = mmap(0, cur_map_len, PROT_READ, MAP_SHARED, fd, cur_map_off);
		if(mm == MAP_FAILED) {
		    log_err("mmap failed after seek: %s", strerror(errno));	
		    ret = LIBLOSSLESS_ERR_INIT;
		    goto done;	
		}	
		mptr = mm + (off & pg_mask);
		mend = mm + cur_map_len;	
		i = (mend - mptr < read_bytes) ? mend - mptr : read_bytes;
		log_info("remapped");
	    }

	    if(format->fmt == SNDRV_PCM_FORMAT_S24_LE) {
		int8_t *src = (int8_t *) mptr;
		int8_t *dst = (int8_t *) pcmbuf;

#if defined(__ARM_ARCH_7A__)
	      if(optimise) convert24_asm(dst, src, i/12);
	      else
#endif
		for(k = 0; k < i/3; k++) {
		    dst[0] = src[0];	
		    dst[1] = src[1];	
		    dst[2] = src[2];	
		    dst[3] = (src[2] < 0) ? 0xff : 0;
		    src += 3; dst += 4;
		}
	    }

	    pthread_mutex_lock(&ctx->mutex);
            switch(ctx->state) {
                case STATE_PLAYING:		/* for S24_LE, bytes are copied to pcmbuf (= alsa priv->buf) above */
		    if(own_buf) k = alsa_write_mmapped(ctx, pcmbuf, i/b2f);
		    else if(format->fmt == SNDRV_PCM_FORMAT_S24_LE) k = alsa_write(ctx, 0, i/b2f);
		    else k = alsa_write(ctx, mptr, i/b2f); /* otherwise supply direct pointer to mmapped space*/		
                    break;
                case STATE_PAUSED:
                    pthread_mutex_unlock(&ctx->mutex);
                    continue;
                case STATE_STOPPED:
                    pthread_mutex_unlock(&ctx->mutex);
                    log_info("stopped before write");
		    goto done;
                default:
                    pthread_mutex_unlock(&ctx->mutex);
                    log_err("cannot happen");   
		    goto done;	
            }
	    pthread_mutex_unlock(&ctx->mutex);
	    if(k <= 0) {
		if(ctx->alsa_error) ret = LIBLOSSLESS_ERR_IO_WRITE;
		break;
	    }
	    mptr += i;	
	}

    done:
	if(fd >= 0) close(fd);
	if(mm != MAP_FAILED) munmap(mm, cur_map_len);
	if(own_buf && pcmbuf) free(pcmbuf);

	audio_stop(ctx, 0);
	if(ret == 0) {
	    gettimeofday(&tstop,0);
	    timersub(&tstop, &tstart, &tdiff);
	    log_info("playback complete in %ld.%03ld sec", tdiff.tv_sec, tdiff.tv_usec/1000);	
	} else log_info("stopping playback on error: ret=%d, err=%d", ret, ctx->alsa_error);
	
    return ret;


}


