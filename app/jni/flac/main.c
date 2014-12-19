/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 Dave Chapman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#ifdef ANDROID
#include <android/log.h>
#include <jni.h>
#else
#include <jni_sub.h>
#endif
#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

#include "flac/decoder.h"
#include "main.h"

#ifndef log_err
#define log_err(fmt, args...)   fprintf(stderr, fmt "\n", ##args)
#define log_info(fmt, args...)  fprintf(stdout, fmt "\n", ##args)
#endif

#define MAX_SUPPORTED_SEEKTABLE_SIZE 5000

static void flac_exit(FLACContext* fc)
{	
    int i;
    if(fc) {
	if(fc->seekpoints) free(fc->seekpoints);
	for(i = 0; i < MAX_CHANNELS; i++) if(fc->decoded[i]) free(fc->decoded[i]);
	free(fc);
    }
}

static FLACContext *flac_init(unsigned char *mmap_addr)
{
    bool found_streaminfo = false;
    uint32_t seekpoint_hi,seekpoint_lo;
    uint32_t offset_hi,offset_lo;
    uint16_t blocksize, max_blocksize;
    int k, endofmetadata = 0;
    uint32_t blocklength;
    FLACContext *fc = 0;
    unsigned char *mptr = mmap_addr, *c;

	fc = calloc(1, sizeof(FLACContext));
	if(!fc) return fc;

	if(memcmp(mptr, "fLaC", 4) != 0) {
	    if(memcmp(mptr, "ID3",3) !=0) {
		log_err("flac signature not found");
		flac_exit(fc);
		return 0;
	    }
	    fc->metadatalength = 10;		/* ID3X[4]+len[6] */
	    fc->metadatalength += mptr[6] << 21;
	    fc->metadatalength += mptr[7] << 14;
	    fc->metadatalength += mptr[8] << 7;
	    fc->metadatalength += mptr[9];
	    mptr += fc->metadatalength;		/* [len] */ 
	    if(memcmp(mptr, "fLaC", 4) != 0) {
		log_err("flac signature not found");
		flac_exit(fc);
		return 0;
	    }
	    fc->metadatalength += 4; /* fLaC[4]  */
	} else fc->metadatalength = 4;
	
	mptr += 4;	
	
	for(k = 0; k < MAX_CHANNELS; k++) {
	    fc->decoded[k] = (int32_t *) calloc(MAX_BLOCKSIZE, sizeof(int32_t));
	    if(!fc->decoded[k])	{
		log_err("no memory for decoded blocks");
		flac_exit(fc);
		return 0;
	    }	
	}

	fc->seekpoints = calloc(MAX_SUPPORTED_SEEKTABLE_SIZE, sizeof(struct FLACseekpoint));
	if(!fc->seekpoints) {
	    log_err("no memory for seekpoints");
	    flac_exit(fc);
	    return 0;
	}


	while(!endofmetadata) {

	    endofmetadata = (mptr[0] & 0x80);
	    blocklength = (mptr[1] << 16) | (mptr[2] << 8) | mptr[3];
	    fc->metadatalength += (blocklength + 4);

	    if((mptr[0] & 0x7f) == 0) {
	
		mptr += 4;
		max_blocksize = (mptr[2] << 8) | mptr[3];
		fc->min_blocksize = (mptr[0] << 8) | mptr[1];

		if(max_blocksize > MAX_BLOCKSIZE) {
		    log_err("FLAC: Maximum blocksize is too large (%d > %d)\n", max_blocksize, MAX_BLOCKSIZE);
		    flac_exit(fc);
		    return 0;
		}

		fc->max_blocksize = max_blocksize;
		fc->min_framesize = (mptr[4] << 16) | (mptr[5] << 8) | mptr[6];
		fc->max_framesize = (mptr[7] << 16) | (mptr[8] << 8) | mptr[9];
		fc->samplerate = (mptr[10] << 12) | (mptr[11] << 4) | ((mptr[12] & 0xf0) >> 4);
		fc->channels = ((mptr[12] & 0x0e) >> 1) + 1;
		fc->bps = (((mptr[12] & 0x01) << 4) | ((mptr[13] & 0xf0) >> 4) ) + 1;

		/* totalsamples is a 36-bit field, but we assume <= 32 bits are used */
		fc->totalsamples = (mptr[14] << 24) | (mptr[15] << 16) | (mptr[16] << 8) | mptr[17];

		/* Calculate track length (in ms) and estimate the bitrate (in kbit/s) */
		fc->length = ((int64_t) fc->totalsamples * 1000) / fc->samplerate;
		found_streaminfo = true;

	    } else if((mptr[0] & 0x7f) == 3) {

		mptr += 4;
		for(k = 0; (fc->nseekpoints < MAX_SUPPORTED_SEEKTABLE_SIZE) && (k < blocklength); k += 18) {		
		    c = mptr + k;	
		    seekpoint_hi = (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
		    seekpoint_lo = (c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7];
		    offset_hi = (c[8] << 24) | (c[9] << 16) | (c[10] << 8) | c[11];
		    offset_lo = (c[12] << 24) | (c[13] << 16) | (c[14] << 8) | c[15];
		    blocksize = (c[16] << 8) | c[17];
		    /* Only store seekpoints where the high 32 bits are zero */
		    if((seekpoint_hi == 0) && (seekpoint_lo != 0xffffffff) && (offset_hi == 0)) {
			fc->seekpoints[fc->nseekpoints].sample=seekpoint_lo;
			fc->seekpoints[fc->nseekpoints].offset=offset_lo;
			fc->seekpoints[fc->nseekpoints].blocksize=blocksize;
			fc->nseekpoints++;
		    }
		}
	    } else mptr += 4;

	    mptr += blocklength;
	}

	if(!found_streaminfo) {
	    log_err("streaminfo block not found\n");
	    flac_exit(fc);
	    return 0;
	}

    return fc;
}


void yield() { }

/* Synchronize to next frame in stream - adapted from libFLAC 1.1.3b2 */

static bool frame_sync(FLACContext* fc, int fd, void *buff, int buff_size) 
{
    unsigned int x = 0;
    bool cached = false;
    off_t pos;
    int k;	
	/* Make sure we're byte aligned. */
	align_get_bits(&fc->gb);

	while(1) {	/* look for ff ff f8 sync sequence */
   	    if(fc->gb.size_in_bits - get_bits_count(&fc->gb) < 8) return false;

   	    if(cached) cached = false;
   	    else x = get_bits(&fc->gb, 8);

    	    if(x == 0xff) {
		x = get_bits(&fc->gb, 8);
		if(x == 0xff) cached = true;
		else if(x >> 2 == 0x3e) break;
	    }
	}

	/* Advance and init bit buffer to the new frame. */
	pos = (get_bits_count(&fc->gb)-16)>>3;
	if(lseek(fd, pos, SEEK_CUR) < 0) return false;

	k = read(fd, buff, buff_size);
	if(k < 0) return false;

	lseek(fd, -k, SEEK_CUR);

	/* Decode the frame to verify the frame crc and fill fc with its metadata. */
	init_get_bits(&fc->gb, buff, buff_size * 8);

	if(flac_decode_frame(fc, buff, buff_size, yield) < 0) return false;

    return true;

}

/* Seek to sample - adapted from libFLAC 1.1.3b2+ */
static bool flac_seek(FLACContext* fc, int fd, uint32_t target_sample, void *bit_buffer, int buff_size) 
{
    off_t orig_pos, pos = -1;
    unsigned long lower_bound, upper_bound;
    unsigned long lower_bound_sample, upper_bound_sample;
    int i;
    unsigned approx_bytes_per_frame;
    uint32_t this_frame_sample = fc->samplenumber;
    unsigned this_block_size = fc->blocksize;
    bool needs_seek = true, first_seek = true;
    unsigned unparseable_count;
    bool got_a_frame;

    orig_pos = lseek(fd, 0, SEEK_CUR);	
    /* We are just guessing here. */
    if(fc->max_framesize > 0)
        approx_bytes_per_frame = (fc->max_framesize + fc->min_framesize)/2 + 1;
    /* Check if it's a known fixed-blocksize stream. */
    else if(fc->min_blocksize == fc->max_blocksize && fc->min_blocksize > 0)
        approx_bytes_per_frame = fc->min_blocksize*fc->channels*fc->bps/8 + 64;
    else
        approx_bytes_per_frame = 4608 * fc->channels * fc->bps/8 + 64;

    /* Set an upper and lower bound on where in the stream we will search. */
    lower_bound = fc->metadatalength;
    lower_bound_sample = 0;
    upper_bound = fc->filesize;
    upper_bound_sample = fc->totalsamples>0 ? fc->totalsamples : target_sample;

    /* Refine the bounds if we have a seektable with suitable points. */
    if(fc->nseekpoints > 0) {
        /* Find the closest seek point <= target_sample, if it exists. */
        for(i = fc->nseekpoints-1; i >= 0; i--) {
            if(fc->seekpoints[i].sample <= target_sample)
                break;
        }
        if(i >= 0) { /* i.e. we found a suitable seek point... */
            lower_bound = fc->metadatalength + fc->seekpoints[i].offset;
            lower_bound_sample = fc->seekpoints[i].sample;
        }
        /* Find the closest seek point > target_sample, if it exists. */
        for(i = 0; i < fc->nseekpoints; i++) {
            if(fc->seekpoints[i].sample > target_sample)
                break;
        }
        if(i < fc->nseekpoints) { /* i.e. we found a suitable seek point... */
            upper_bound = fc->metadatalength + fc->seekpoints[i].offset;
            upper_bound_sample = fc->seekpoints[i].sample;
        }
    }

    while(1) {

        /* Check if bounds are still ok. */
	if(lower_bound_sample >= upper_bound_sample || lower_bound > upper_bound) return false;
     
        /* Calculate new seek position */
	if(needs_seek) {
	    pos = (off_t) (lower_bound +
		 (((target_sample - lower_bound_sample) * (int64_t) (upper_bound - lower_bound)) /
		  (upper_bound_sample - lower_bound_sample)) - approx_bytes_per_frame);
	    if(pos >= (off_t)upper_bound) pos = (off_t) upper_bound-1;
	    if(pos < (off_t)lower_bound) pos = (off_t) lower_bound;
	}
	if(lseek(fd, pos, SEEK_SET) < 0) return false;

	if(read(fd, bit_buffer, buff_size) < 0) return false;
	init_get_bits(&fc->gb, bit_buffer, buff_size*8);

	if(lseek(fd, pos, SEEK_SET) < 0) return false;

	/* Get a new frame */
	for(got_a_frame = false, unparseable_count = 0; !got_a_frame
	    && unparseable_count < 30; unparseable_count++) 
		if(frame_sync(fc, fd, bit_buffer, buff_size)) got_a_frame = true;
	
	if(!got_a_frame) goto seek_err;
	

	this_frame_sample = fc->samplenumber;
	this_block_size = fc->blocksize;

	if(target_sample >= this_frame_sample
	    && target_sample < this_frame_sample + this_block_size) {
	    /* Found the frame containing the target sample. */
	    fc->sample_skip = target_sample - this_frame_sample;
	    break;
	}

	if(this_frame_sample + this_block_size >= upper_bound_sample && !first_seek) {
	    if(pos == (off_t)lower_bound || !needs_seek) goto seek_err;
	    /* Our last move backwards wasn't big enough, try again. */
	    approx_bytes_per_frame *= 2;
	    continue;
	}

	/* Allow one seek over upper bound, required for streams with unknown total samples. */
	first_seek = false;

	/* Make sure we are not seeking in a corrupted stream */
	if(this_frame_sample < lower_bound_sample) goto seek_err;

	approx_bytes_per_frame = this_block_size * fc->channels * fc->bps/8 + 64;

	/* We need to narrow the search. */
	if(target_sample < this_frame_sample) {
	    upper_bound_sample = this_frame_sample;
	    upper_bound = (unsigned long) lseek(fd,0,SEEK_CUR);
	} else { /* Target is beyond this frame. */
	    /* We are close, continue in decoding next frames. */
	    if(target_sample < this_frame_sample + 4*this_block_size) {
		pos = (unsigned long) lseek(fd,0,SEEK_CUR) + fc->framesize;
		needs_seek = false;
	    }
	    lower_bound_sample = this_frame_sample + this_block_size;
	    lower_bound = (unsigned long) lseek(fd,0,SEEK_CUR) + fc->framesize;
	}
    }
    return true;

seek_err:
    lseek(fd, orig_pos, SEEK_SET);
    return false;	
}

/* 128 Mb not too much for 192/24 flacs, yeah? */
#define MMAP_SIZE	(128*1024*1024)

int flac_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start)
{
    int i, k, phys_bps, scale, ret = 0, fd = -1;
    void *mptr, *mend, *mm = MAP_FAILED;
    FLACContext *fc = 0;
    int32_t *src, *dst; 
    int16_t *dst16;   
    void *pcmbuf = 0; 
    const char *file = 0;
    off_t flen = 0; 
    off_t off, cur_map_off; /* file offset currently mapped to mm */
    size_t cur_map_len;	
    const off_t pg_mask = sysconf(_SC_PAGESIZE) - 1;    
	
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

	fc = flac_init(mm);
	if(!fc) {
	    ret = LIBLOSSLESS_ERR_INIT;
	    goto done;	
	}
	/* needed for seek */
	fc->filesize = flen;
	fc->bitrate = ((int64_t) (fc->filesize - fc->metadatalength) * 8) / fc->length;

	ctx->samplerate = fc->samplerate;
	ctx->channels = fc->channels;	
	ctx->bps = fc->bps;
	ctx->track_time = fc->totalsamples / fc->samplerate;
	ctx->block_min = fc->min_blocksize;
	ctx->block_max = fc->max_blocksize;

    	log_info("Source: %d-bit %d-channel %d Hz max_block=%d time=%d", 
		fc->bps, fc->channels, fc->samplerate, fc->max_blocksize, ctx->track_time);

	/* set starting frame */
        if(start) {
	    int bsize = MAX_FRAMESIZE+16;
	    void *bbuff = malloc(bsize);
	    if(!bbuff) {
		ret = LIBLOSSLESS_ERR_NOMEM;
		goto done; 
	    }
	    lseek(fd, fc->metadatalength, SEEK_SET);	
	    if(!flac_seek(fc, fd, start * fc->samplerate, bbuff, bsize)) {
		if(!flac_seek(fc, fd, (start + 1) * fc->samplerate, bbuff, bsize)) {  /* helps sometimes */
		    free(bbuff);
		    ret = LIBLOSSLESS_ERR_OFFSET;
		    log_err("seek to %d sec failed", start);
		    goto done; 
		}
	    }
	    free(bbuff);
	    munmap(mm, cur_map_len);
	    off = lseek(fd, 0, SEEK_CUR);
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
	    mptr = mm + fc->metadatalength;
	    mend = mm + cur_map_len;
	}

	ret = audio_start(ctx);
	if(ret) goto done;

	scale = FLAC_OUTPUT_DEPTH - fc->bps;
	phys_bps = ctx->format->phys_bits;  /* ctx->format selected in alsa_start() */
	
	pcmbuf = malloc(fc->channels * (phys_bps/8) * MAX_BLOCKSIZE);
	if(!pcmbuf) {
	    log_err("no memory");
	    ret = LIBLOSSLESS_ERR_NOMEM;	
	    goto done;	
	}

	update_track_time(env, obj, ctx->track_time);

	while(mptr < mend) {

	    i = (mend - mptr < MAX_FRAMESIZE) ? mend - mptr : MAX_FRAMESIZE;
	    if(i < MAX_FRAMESIZE && cur_map_off + cur_map_len != flen) {	/* too close to end of mapped region, but not at eof */
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
		log_info("remapped");
	    }

	    k = flac_decode_frame(fc, mptr, i, yield);
	    if(k < 0) {
		if((unsigned int) (mend - mptr) < 0x1000) {
		    log_info("garbage at EOF skipped");
		    goto done;	
		}
		log_err("decoder error %d", k);
		ret = LIBLOSSLESS_ERR_DECODE;
		goto done;
	    }
	    mptr += fc->gb.index/8; /* step over the bytes consumed by flac_decode_frame() */	
	    
	    switch(ctx->format->fmt) {

		case SNDRV_PCM_FORMAT_S32_LE:
		    for(i = 0; i < fc->channels; i++) {
			src = fc->decoded[i];
			dst = (int32_t *) pcmbuf + i;
			for(k = 0; k < fc->blocksize; k++)  {
			    *dst = *src++;
			     dst += fc->channels;	
			}
		    }
		    break;	

		case SNDRV_PCM_FORMAT_S24_3LE:
		    for(i = 0; i < fc->channels; i++) {
			uint8_t *dst8 = (uint8_t *) pcmbuf + i * 3;
			src = fc->decoded[i];
			for(k = 0; k < fc->blocksize; k++) {
			     uint32_t y = (uint32_t) (*src++ >> scale); 
			     dst8[0] = (uint8_t) y;
			     dst8[1] = (uint8_t) (y >> 8);
			     dst8[2] = (uint8_t) (y >> 16);
			     dst8 += fc->channels * 3;
			}
		    }
		    break;

		case SNDRV_PCM_FORMAT_S24_LE:
		    for(i = 0; i < fc->channels; i++) {
			src = fc->decoded[i];
			dst = (int32_t *) pcmbuf + i;
			for(k = 0; k < fc->blocksize; k++) {
			    *dst = (*src++ >> scale);
			     dst += fc->channels;
			}
		    }
		    break;

		case SNDRV_PCM_FORMAT_S16_LE:		
		    for(i = 0; i < fc->channels; i++) {
			src = fc->decoded[i];
			dst16 = (int16_t *) pcmbuf + i;
		    	for(k = 0; k < fc->blocksize; k++) {
			    *dst16 = (int16_t) (*src++ >> scale);
			     dst16 += fc->channels;
			}
		    }
		    break;	
		default:
		    log_err("internal error: format not supported");
		    ret = LIBLOSSLESS_ERR_INIT;
		    goto done; 	
	    }
	    i = audio_write(ctx, pcmbuf, fc->blocksize * fc->channels * (phys_bps/8));
	    if(i < 0) {
		if(ctx->alsa_error) ret = LIBLOSSLESS_ERR_IO_WRITE;
		break;
	    }	
	}

    done:
	if(fc) flac_exit(fc);
	if(pcmbuf) free(pcmbuf);
#ifdef ANDROID
	if(file) (*env)->ReleaseStringUTFChars(env,jfile,file);
#endif
	if(fd >= 0) close(fd);
	if(mm != MAP_FAILED) munmap(mm, cur_map_len);

	log_info("stopping audio on exit");
	audio_stop(ctx, 0);
	log_info("exiting, ret=%d, err=%d", ret, ctx->alsa_error);
	
    return ret;
}


#ifdef ANDROID
#ifdef SWAP32
#undef SWAP32
#endif
#define SWAP32(A,B) (((A)[(B)+0]) << 24) | (((A)[(B)+1]) << 16) | (((A)[(B)+2]) << 8) | (((A)[(B)+3]) << 0) 
static int *flac_read_cue(int fd)
{
    unsigned char buf[255];
    int endofmetadata = 0;
    uint32_t blocklength, *times, samplerate = 0, off_lo, off_hi;
    uint8_t *p, i, k, j = 0, n, *cue_buf = 0;
    uint64_t k1, k2;

    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    if (read(fd, buf, 4) < 4) return 0;
    if (memcmp(buf,"fLaC",4) != 0) {

        if(memcmp(buf, "ID3",3) !=0) return 0;
        if (read(fd, buf, 6) < 6) return 0;
        blocklength = buf[2] << 21;
        blocklength += buf[3] << 14;
        blocklength += buf[4] << 7;
        blocklength += buf[5];
        blocklength += 10;
        if (lseek(fd, blocklength, SEEK_SET) < 0) return 0;
        if (read(fd, buf, 4) < 4) return 0;
        if (memcmp(buf,"fLaC",4) != 0) return 0;
    }
    while (!endofmetadata) {
        if (read(fd, buf, 4) < 4) return 0;
        endofmetadata=(buf[0]&0x80);
        blocklength = (buf[1] << 16) | (buf[2] << 8) | buf[3];

        if ((buf[0] & 0x7f) == 0)  { /* STREAMINFO */
            if (read(fd, buf, blocklength) < 0) return 0;
            samplerate = (buf[10] << 12) | (buf[11] << 4) | ((buf[12] & 0xf0) >> 4);
	} else if ((buf[0] & 0x7f) == 5) { /* BLOCK_CUESHEET */
	    if(!samplerate) return 0;	// STREAMINFO must be present as the first metadata block 
	    cue_buf = (unsigned char *)	malloc(blocklength);
	    if(!cue_buf) return 0;	
            if(read(fd, cue_buf, blocklength) != blocklength) return 0;
#define SIZEOF_METADATA_BLOCK_CUESHEET	 (128+8+1+258+1)
#define SIZEOF_CUESHEET_TRACK		 (8+1+12+1+13+1)
#define SIZEOF_CUESHEET_TRACK_INDEX	 (8+1+3)
	    p = cue_buf + SIZEOF_METADATA_BLOCK_CUESHEET; // now pointing to CUESHEET_TRACK
	    n = p[-1];
	    times = (uint32_t *) malloc((n+1) * sizeof(uint32_t));	
	    if(!times) {
		free(cue_buf); return 0;
	    }		
//	__android_log_print(ANDROID_LOG_ERROR,"liblossless","Found CUE block, %d tracks", n);
	    memset(times,0,(n+1)*sizeof(uint32_t));	
	    for(i = 0, j = 0; i < n && j < n; i++) {
		// CUESHEET_TRACK
	      uint8_t idx_points, track_no = p[8];
		off_hi = SWAP32(p,0);
		off_lo = SWAP32(p,4);
		k1 = (((uint64_t) off_hi) << 32) |((uint64_t) off_lo);				    	
		p += SIZEOF_CUESHEET_TRACK; // now pointing to the first CUE_TRACK_INDEX
		idx_points = p[-1];
		if(track_no >= 0 && track_no <= 99) {
		   uint8_t *p1 = p;
		    for(k = 0; k < idx_points; k++) {	
			if(p1[8] == 1) {	// Save INDEX 01 records only!
	                    off_hi = SWAP32(p1,0);
        	            off_lo = SWAP32(p1,4);
                	    k2 = (((uint64_t) off_hi) << 32) |((uint64_t) off_lo);
			    times[++j] = (uint32_t)((k2+k1)/samplerate);
//	__android_log_print(ANDROID_LOG_ERROR,"liblossless","Found CUE index 01 at %d", times[j-1]);
			    break; 		
			}
			p1 += SIZEOF_CUESHEET_TRACK_INDEX;
		    }	
		} 
		p += idx_points*SIZEOF_CUESHEET_TRACK_INDEX;  
	    }				
	    free(cue_buf);
	    if(j == 0) {
		free(times); return 0;
	    }	
	    times[0] = (uint32_t) j;
//	__android_log_print(ANDROID_LOG_ERROR,"liblossless","Returning array, len=%d", j);
	    return (int *) times;	
        } else {
            /* Skip to next metadata block */
            if (lseek(fd, blocklength, SEEK_CUR) < 0)
            {
                return 0;
            }
        }
    }
    return 0;
}

jintArray JNICALL extract_flac_cue(JNIEnv *env, jobject obj, jstring jfile) {

  const char *file = (*env)->GetStringUTFChars(env,jfile,NULL);
  int fd;	
  int *k = 0, n;
  jintArray ja = 0;
    if(!file) {
        (*env)->ReleaseStringUTFChars(env,jfile,file);  return 0;
    }
    fd = open(file,O_RDONLY);
    (*env)->ReleaseStringUTFChars(env,jfile,file);
    if(fd < 0 || (k = flac_read_cue(fd)) == 0 || (n = k[0]) == 0) {
	if(k) free(k);
	close(fd);
	return 0;
    }	
    close(fd);
    ja = (*env)->NewIntArray(env,n);
    (*env)->SetIntArrayRegion(env,ja,0,n,(jint *)(k+1));    
    free(k);	
    return ja;
}
#endif


