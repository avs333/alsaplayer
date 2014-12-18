/*

demac - A Monkey's Audio decoder

$Id: demac.c 19517 2008-12-21 01:29:36Z amiconn $

Copyright (C) Dave Chapman 2007

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110, USA

*/

/* 

This example is intended to demonstrate how the decoder can be used in
embedded devices - there is no usage of dynamic memory (i.e. no
malloc/free) and small buffer sizes are chosen to minimise both the
memory usage and decoding latency.

This implementation requires the following memory and supports decoding of all APE files up to 24-bit Stereo.

32768 - data from the input stream to be presented to the decoder in one contiguous chunk.
18432 - decoding buffer (left channel)
18432 - decoding buffer (right channel)

17408+5120+2240 - buffers used for filter histories (compression levels 2000-5000)

In addition, this example uses a static 27648 byte buffer as temporary
storage for outputting the data to a WAV file but that could be
avoided by writing the decoded data one sample at a time.

*/

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include "demac.h"
#include "../jni_sub.h"
#include "../main.h"
#include <sys/mman.h>
#ifdef ANDROID
#include <android/log.h>
#endif

#define __force
#define __bitwise
#define __user
#include <sound/asound.h>


#define BLOCKS_PER_LOOP     4608
#define MAX_CHANNELS        2
#define MAX_BYTESPERSAMPLE  3

#define INPUT_CHUNKSIZE     (32*1024)

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif


static int ape_calc_seekpos(struct ape_ctx_t* ape_ctx, 	
	uint32_t new_sample, uint32_t* newframe, uint32_t* filepos, uint32_t* samplestoskip)
{
    uint32_t n;

    n = new_sample / ape_ctx->blocksperframe;
    if (n >= ape_ctx->numseekpoints)
    {
        /* We don't have a seekpoint for that frame */
        return 0;
    }

    *newframe = n;
    *filepos = ape_ctx->seektable[n];
    *samplestoskip = new_sample - (n * ape_ctx->blocksperframe);

    return 1;
}

#define MMAP_SIZE       (128*1024*1024)

int ape_play(JNIEnv *env, jobject obj, playback_ctx* ctx, jstring jfile, int start) 
{
    int currentframe, nblocks, bytesconsumed, bytesperblock;
    int bytesinbuffer, blockstodecode, firstbyte;
    int fd = -1, i = 0, n, bytes_to_write;

    int32_t  sample32;
   
    unsigned char inbuffer[INPUT_CHUNKSIZE];
    int32_t *decoded[2] = { 0, 0 };
    uint8_t *p, *pcmbuf = 0;	

    struct ape_ctx_t ape_ctx;
    uint32_t samplestoskip;
    const char *file = 0;
    int ret = 0;
	
    off_t flen = 0;
    off_t off, cur_map_off;	/* file offset currently mapped to mm */
    size_t cur_map_len;		/* size of file chunk currently mapped */
    const off_t pg_mask = getpagesize() - 1;
    void *mptr, *mend, *mm = MAP_FAILED;
   
    int ape_read(void *buff, int nbytes) 
    {
	if(mptr + nbytes > mend) {
	    if(cur_map_off + cur_map_len == flen) nbytes = mend - mptr;  /* at EOF, no further remapping possible */
	    else {  /* remap */
		munmap(mm, cur_map_len);
		off = mptr - mm;
		cur_map_off = (cur_map_off + off) & ~pg_mask;
		cur_map_len = (flen - cur_map_off) > MMAP_SIZE ? MMAP_SIZE : flen - cur_map_off;
		mm = mmap(0, cur_map_len, PROT_READ, MAP_SHARED, fd, cur_map_off);
		if(mm == MAP_FAILED) {
		    log_err("mmap failed in %s: %s", __func__, strerror(errno));
		    return -1;
		}
		mptr = mm + (off & pg_mask);
		mend = mm + cur_map_len;
		log_info("remapped");
		if(mptr + nbytes > mend) {
		    log_err("requested too much in %s: [%d]", __func__, nbytes);
		    return -1;
		}
	    }
	}
	memcpy(buff, mptr, nbytes);
	mptr += nbytes;
	return nbytes;
    }	

//#define TESTX 1
#ifdef TESTX
   int fdx;
   int qqq = 0;
   int must = 1;
#ifdef ANDROID
	fdx = open("/sdcard/qqq.out", O_WRONLY|O_CREAT|O_TRUNC, 0666);
#else
	fdx = open("qqq.out", O_WRONLY|O_CREAT|O_TRUNC, 0666);
#endif
	if(fdx < 0) log_err("cannot open!!!!!!!");
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

	decoded[0] = (int32_t *) malloc(BLOCKS_PER_LOOP * sizeof(int32_t));
	decoded[1] = (int32_t *) malloc(BLOCKS_PER_LOOP * sizeof(int32_t));
	pcmbuf = (uint8_t *) malloc(2 * BLOCKS_PER_LOOP * sizeof(int32_t));

	if(!pcmbuf || !decoded[0] || !decoded[1]) {
	    log_err("no memory"); 	
	    ret = LIBLOSSLESS_ERR_NOMEM;
	    goto done;	  
	}

	/* Read the file headers to populate the ape_ctx struct */

	if(read(fd, inbuffer, INPUT_CHUNKSIZE) != INPUT_CHUNKSIZE) {
	    log_err("error reading headers");
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto done;
	}

	if(ape_parseheaderbuf(inbuffer, &ape_ctx) < 0) {
	    log_err("error parsing headers");
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;	
	}

	if ((ape_ctx.fileversion < APE_MIN_VERSION) || (ape_ctx.fileversion > APE_MAX_VERSION)) {
	    log_err("unsupported APE version");
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;
	}

	if(start) {
	    ape_ctx.seektable = (uint32_t *) malloc(ape_ctx.seektablelength);
	    if(!ape_ctx.seektable) {
		log_err("no memory for seektable");	
		ret = LIBLOSSLESS_ERR_NOMEM;
		goto done;
	    }
	    if(lseek(fd, ape_ctx.seektablefilepos, SEEK_SET) < 0) {
		log_err("ape not seekable");	
		ret = LIBLOSSLESS_ERR_FORMAT;
		free(ape_ctx.seektable);
		goto done;	
	    }
	    if(read(fd, ape_ctx.seektable, ape_ctx.seektablelength) != ape_ctx.seektablelength) {
		log_err("cannot read seektable");	
		ret = LIBLOSSLESS_ERR_FORMAT;
		free(ape_ctx.seektable);
		goto done;	
	    }
	    /* start_sample = ape_ctx.samplerate * start; */
	    if(ape_calc_seekpos(&ape_ctx, start * ape_ctx.samplerate,
			(uint32_t *) &currentframe, (uint32_t *) &off,&samplestoskip) == 0) {
		log_err("failed to determine seek offset");	
		ret = LIBLOSSLESS_ERR_OFFSET;
		free(ape_ctx.seektable);
		goto done;
	    }
	    free(ape_ctx.seektable);
	    ape_ctx.seektable = 0;
            firstbyte = 3 - (off & 3);
            off &= ~3;
	} else {
	    off = ape_ctx.firstframe;
	    samplestoskip = 0;
	    currentframe = 0;
	    firstbyte = 3;
	}

	if(ape_ctx.channels != 2 || (ape_ctx.bps != 16 && ape_ctx.bps != 24)) {
	    log_err("unsupported ape: channels %d, bps %d", ape_ctx.channels, ape_ctx.bps);
	    ret = LIBLOSSLESS_ERR_FORMAT;
	    goto done;
	}

	ctx->channels = ape_ctx.channels;
	ctx->samplerate = ape_ctx.samplerate;
	ctx->bps = ape_ctx.bps;
	ctx->written = 0;
 	ctx->track_time = ape_ctx.totalsamples/ape_ctx.samplerate;
	ctx->block_max = ctx->block_min = 4096;

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

	ret = audio_start(ctx);
	if(ret != 0) goto done;
	
        update_track_time(env,obj,ctx->track_time);

	bytes_to_write = 0;
	bytesperblock = (ctx->format->phys_bits/8) * ctx->block_max;

	/* Initialise the buffer */
	bytesinbuffer = ape_read(inbuffer, INPUT_CHUNKSIZE);
	if(bytesinbuffer < 0) {
	    log_err("read error");
	    ret = LIBLOSSLESS_ERR_IO_READ;
	    goto done;
	}	

	/* The main decoding loop - we decode the frames a small chunk at a time */
	while(currentframe < ape_ctx.totalframes) {

	       /* Calculate how many blocks there are in this frame */
	    nblocks = (currentframe == (ape_ctx.totalframes - 1)) ? 
			ape_ctx.finalframeblocks : ape_ctx.blocksperframe;

	    ape_ctx.currentframeblocks = nblocks;

	    /* Initialise the frame decoder */
	    init_frame_decoder(&ape_ctx, inbuffer, &firstbyte, &bytesconsumed);

	    /* Update buffer */
	    memmove(inbuffer, inbuffer + bytesconsumed, bytesinbuffer - bytesconsumed);
	    bytesinbuffer -= bytesconsumed;

	    n = ape_read(inbuffer + bytesinbuffer, INPUT_CHUNKSIZE - bytesinbuffer);
	    if(n < 0) {
		log_err("read error");
		ret = LIBLOSSLESS_ERR_IO_READ;
		goto done;
	    }	
	    bytesinbuffer += n;

	    /* Decode the frame a chunk at a time */
	    while(nblocks > 0) {

		blockstodecode = MIN(BLOCKS_PER_LOOP, nblocks);

		if(decode_chunk(&ape_ctx, inbuffer, &firstbyte,
			&bytesconsumed, decoded[0], decoded[1], blockstodecode) < 0)  {
		    log_err("decoder error");
		    ret = LIBLOSSLESS_ERR_DECODE;
		    goto done;
		}

	        if(samplestoskip) {
		    log_info("samplestoskip %d", samplestoskip);		
		    if(samplestoskip >= blockstodecode) {
			samplestoskip -= blockstodecode;
			memmove(inbuffer, inbuffer + bytesconsumed, bytesinbuffer - bytesconsumed);
			bytesinbuffer -= bytesconsumed;
			n = ape_read(inbuffer + bytesinbuffer, INPUT_CHUNKSIZE - bytesinbuffer);
			if(n < 0) {
			    log_err("read error");
			    ret = LIBLOSSLESS_ERR_IO_READ;
			    goto done;
			}
			bytesinbuffer += n;
			nblocks -= blockstodecode;
			continue;
		    }
		    nblocks -= samplestoskip;
		    blockstodecode -= samplestoskip;
		}

		/* Convert the output samples to WAV format and write to output file */
		p = pcmbuf + bytes_to_write;

		switch(ctx->format->fmt) {

		    case SNDRV_PCM_FORMAT_S24_3LE:
			for(i = samplestoskip; i < blockstodecode; i++) {
			    sample32 = decoded[0][i];
			    *p++ = sample32 & 0xff;
			    *p++ = (sample32 >> 8) & 0xff;
			    *p++ = (sample32 >> 16) & 0xff;
			    sample32 = decoded[1][i];
			    *p++ = sample32 & 0xff;
			    *p++ = (sample32 >> 8) & 0xff;
			    *p++ = (sample32 >> 16) & 0xff;
			}
			break;

		    case SNDRV_PCM_FORMAT_S24_LE:
			for(i = samplestoskip; i < blockstodecode; i++) {
			    *((int32_t *) p) = decoded[0][i]; p += 4;
			    *((int32_t *) p) = decoded[1][i]; p += 4;
			}
			break;

		    case SNDRV_PCM_FORMAT_S16_LE:
			for(i = samplestoskip; i < blockstodecode; i++) {
			    *((int16_t *) p) = decoded[0][i]; p += 2;
			    *((int16_t *) p) = decoded[1][i]; p += 2;
			}
			break;
		    default:
			ret = LIBLOSSLESS_ERR_INIT;
			goto done;
		}

		if(samplestoskip) samplestoskip = 0;

		n = p - pcmbuf;

		if(n >= bytesperblock) {
		    p = pcmbuf;
		    do {
			i = audio_write(ctx, p, bytesperblock);
#ifdef TESTX
			if(must) {
				if(write(fdx, p, bytesperblock) < 0) log_err("TEST error");
				if(qqq == 200) {
				    must = 0;
				    close(fdx);		
				}
				log_err("TEST: written");
				qqq++;
			}
#endif
			if(i < 0) {
			    if(ctx->alsa_error) ret = LIBLOSSLESS_ERR_IO_WRITE;
			    goto done;
			}
			n -= bytesperblock;
			p += bytesperblock;
		    } while(n >= bytesperblock);
		    memmove(pcmbuf,p,n);
		}

		bytes_to_write = n;

		/* Update the buffer */
		memmove(inbuffer, inbuffer + bytesconsumed, bytesinbuffer - bytesconsumed);
		bytesinbuffer -= bytesconsumed;

		n = ape_read(inbuffer + bytesinbuffer, INPUT_CHUNKSIZE - bytesinbuffer);

		if(n < 0) {
		    log_err("read error");
		    ret = LIBLOSSLESS_ERR_IO_READ;
		    goto done;
		}
	
		bytesinbuffer += n;

		/* Decrement the block count */
		nblocks -= blockstodecode;

	    }  /* while(nblocks > 0)*/
	    currentframe++;
	}  /* currentframe < ape_ctx.totalframes */

    done:
	if(decoded[0]) free(decoded[0]);
	if(decoded[1]) free(decoded[1]);
	if(pcmbuf) free(pcmbuf);
#ifdef ANDROID
	if(file) (*env)->ReleaseStringUTFChars(env,jfile,file);
#endif
	if(fd >= 0) close(fd);
	if(mm != MAP_FAILED) munmap(mm, cur_map_len);

	log_info("exiting, ret=%d, err=%d", ret, ctx->alsa_error);
	audio_stop(ctx, 0);

    return ret;
}


