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
#include "sound/compress_params0102.h"
#include "sound/compress_offload0102.h"


#define BSWAP16(x) (x) = (((x << 8) | ((x >> 8) & 0x00ff)))
#define BSWAP32(x) (x) = (((x << 24) | ((x << 8) & 0x00ff0000) | ((x >> 8) & 0x0000ff00) | ((x >> 24) & 0x000000ff)))
#define MMAP_SIZE	(128*1024*1024)


#if 0
static const ALACChannelLayoutTag       ALACChannelLayoutTags[8] =
{
    kALACChannelLayoutTag_Mono,         // C
    kALACChannelLayoutTag_Stereo,               // L R
    kALACChannelLayoutTag_MPEG_3_0_B,   // C L R
    kALACChannelLayoutTag_MPEG_4_0_B,   // C L R Cs
    kALACChannelLayoutTag_MPEG_5_0_D,   // C L R Ls Rs
    kALACChannelLayoutTag_MPEG_5_1_D,   // C L R Ls Rs LFE
    kALACChannelLayoutTag_AAC_6_1,              // C L R Ls Rs Cs LFE
    kALACChannelLayoutTag_MPEG_7_1_B    // C Lc Rc L R Ls Rs LFE    (doc: IS-13818-7 MPEG2-AAC)
};
#endif


int alac_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start) 
{
    int k, ret = 0, fd = -1;
    void *mm = MAP_FAILED;
    const char *file = 0;
    off_t flen = 0, off;
    size_t cur_map_len;
    uint8_t *c, *ce, *cs;
    struct snd_dec_alac alac;

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
	c = (uint8_t *) mm;
	ce = c + cur_map_len;

	if(memcmp(c, "ID3",3) == 0) {
	    off = c[6] << 21;
            off += c[7] << 14;
            off += c[8] << 7;
            off += c[9];
            off += 10;
	    cs = c + off;
	    c = cs;
	} else cs = c;

	while(c < ce - 16)	/* fuck apple:  moov -> trak -> mdia -> minf -> stbl -> stsd -> alac !!! */
	    if(*c++ == 0x24 && *c++ == 'a' && *c++ == 'l' && *c++ == 'a' && *c++ == 'c') {
		c += 4;
		break;
	    }

	if(c >= ce - 16) {
	    log_err("failed to find alac struct in file");
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;		
        }

	log_info("found snd_dec_alac at %lx", (long) ((void*)c - mm));
	memcpy(&alac, c, sizeof(struct snd_dec_alac) - 4);

	for(off = 0, c = cs; c < ce - 16; c += off) {
	    off = (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
	    if(memcmp(c + 4, "mdat", 4) == 0 && off != 8) {
		c += 8;
		off = c - (uint8_t *) mm;
		break;
	    }
	    if(memcmp(c + 4, "ftyp", 4) != 0 && memcmp(c + 4, "moov", 4) != 0 
			&& memcmp(c + 4, "free", 4) != 0) {
		log_err("invalid chunk found");
		ret = LIBLOSSLESS_ERR_FORMAT;
		goto done;
	    }	    
	}

	if(c >= ce - 16) {
	    log_err("failed to start of data block in file");
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;		
        }
	log_info("found data start at %lx", (long) off);

	BSWAP32(alac.frame_length);
	BSWAP16(alac.max_run);
	BSWAP32(alac.max_frame_bytes);
	BSWAP32(alac.avg_bit_rate);
	BSWAP32(alac.sample_rate);
	alac.channel_layout_tag = 0;

	ctx->samplerate = alac.sample_rate;
	ctx->channels = alac.num_channels;
	ctx->bps = alac.bit_depth;
	ctx->bitrate = ctx->samplerate * ctx->channels * ctx->bps;
	k = (ctx->channels * ctx->bps)/8;
	ctx->track_time = flen/(k * ctx->samplerate);
	ctx->alac_cfg = &alac;
	log_info("switching to offload playback rate=%d chans=%d bps=%d brate=%d", 
		ctx->samplerate, ctx->channels, ctx->bps, ctx->bitrate);

	if(start) off += (start * ctx->samplerate * ctx->channels * ctx->bps/8);

	if(lseek(fd, off, SEEK_SET) != off) {
	    ret = LIBLOSSLESS_ERR_OFFSET;
	    log_err("seek to %d sec failed", start);
	    goto done; 
	}
	munmap(mm, cur_map_len);
	update_track_time(env, obj, ctx->track_time);

	return alsa_play_offload(ctx,fd,off);

    done:
	if(fd >= 0) close(fd);
	if(mm != MAP_FAILED) munmap(mm, cur_map_len);
	playback_complete(ctx, __func__);
	
    return ret;

}


	
