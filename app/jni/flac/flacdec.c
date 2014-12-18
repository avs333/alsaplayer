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
//#include <android/log.h>
#include "decoder.h"

#if 0
static FLACContext fc IBSS_ATTR_FLAC;
/* The output buffers containing the decoded samples (channels 0 and 1) */
static int32_t decoded0[MAX_BLOCKSIZE] IBSS_ATTR_FLAC;
static int32_t decoded1[MAX_BLOCKSIZE] IBSS_ATTR_FLAC;
static int32_t decoded2[MAX_BLOCKSIZE] IBSS_ATTR_FLAC_LARGE_IRAM;
static int32_t decoded3[MAX_BLOCKSIZE] IBSS_ATTR_FLAC_LARGE_IRAM;
static int32_t decoded4[MAX_BLOCKSIZE] IBSS_ATTR_FLAC_XLARGE_IRAM;
static int32_t decoded5[MAX_BLOCKSIZE] IBSS_ATTR_FLAC_XLARGE_IRAM;
#endif

#define log_err(fmt, args...)   fprintf(stderr, fmt "\n", ##args)
#define log_info(fmt, args...)  fprintf(stdout, fmt "\n", ##args)

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp) ({         \
    typeof (exp) _rc;                      \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
#endif


#define MAX_SUPPORTED_SEEKTABLE_SIZE 5000

#if 0
/* Notes about seeking:

   The full seek table consists of:
      uint64_t sample (only 36 bits are used)
      uint64_t offset
      uint32_t blocksize

   We also limit the sample and offset values to 32-bits - Rockbox doesn't
   support files bigger than 2GB on FAT32 filesystems.

   The reference FLAC encoder produces a seek table with points every
   10 seconds, but this can be overridden by the user when encoding a file.

   With the default settings, a typical 4 minute track will contain
   24 seek points.

   Taking the extreme case of a Rockbox supported file to be a 2GB (compressed)
   16-bit/44.1KHz mono stream with a likely uncompressed size of 4GB:
      Total duration is: 48694 seconds (about 810 minutes - 13.5 hours)
      Total number of seek points: 4869

   Therefore we limit the number of seek points to 5000.  This is a
   very extreme case, and requires 5000*8=40000 bytes of storage.

   If we come across a FLAC file with more than this number of seekpoints, we
   just use the first 5000.

*/

static int8_t *bit_buffer;
static size_t buff_size;
#endif


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
	    fc->metadatalength = 10; 
	    fc->metadatalength += mptr[6] << 21;
	    fc->metadatalength += mptr[7] << 14;
	    fc->metadatalength += mptr[8] << 7;
	    fc->metadatalength += mptr[9];
	    mptr += fc->metadatalength;
	    if(memcmp(mptr, "fLaC", 4) != 0) {
		log_err("flac signature not found");
		flac_exit(fc);
		return 0;
	    }
	    fc->metadatalength += 4; /* ID3X[4]+len[6]+[len]+fLaC[4]  */
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

#if 0
/* Synchronize to next frame in stream - adapted from libFLAC 1.1.3b2 */
static bool frame_sync(FLACContext* fc) {
    unsigned int x = 0;
    bool cached = false;

    /* Make sure we're byte aligned. */
    align_get_bits(&fc->gb);

    while(1) {
        if(fc->gb.size_in_bits - get_bits_count(&fc->gb) < 8) {
            /* Error, end of bitstream, a valid stream should never reach here
             * since the buffer should contain at least one frame header.
             */
            return false;
        }

        if(cached)
            cached = false;
        else
            x = get_bits(&fc->gb, 8);

        if(x == 0xff) { /* MAGIC NUMBER for first 8 frame sync bits. */
            x = get_bits(&fc->gb, 8);
            /* We have to check if we just read two 0xff's in a row; the second
             * may actually be the beginning of the sync code.
             */
            if(x == 0xff) { /* MAGIC NUMBER for first 8 frame sync bits. */
                cached = true;
            }
            else if(x >> 2 == 0x3e) { /* MAGIC NUMBER for last 6 sync bits. */
                /* Succesfully synced. */
                break;
            }
        }
    }

    /* Advance and init bit buffer to the new frame. */
    ci->advance_buffer((get_bits_count(&fc->gb)-16)>>3); /* consumed bytes */
    bit_buffer = ci->request_buffer(&buff_size, MAX_FRAMESIZE+16);
    init_get_bits(&fc->gb, bit_buffer, buff_size*8);

    /* Decode the frame to verify the frame crc and
     * fill fc with its metadata.
     */
    if(flac_decode_frame(fc, 
       bit_buffer, buff_size, ci->yield) < 0) {
        return false;
    }

    return true;
}

/* Seek to sample - adapted from libFLAC 1.1.3b2+ */
static bool flac_seek(FLACContext* fc, uint32_t target_sample) {
    off_t orig_pos = ci->curpos;
    off_t pos = -1;
    unsigned long lower_bound, upper_bound;
    unsigned long lower_bound_sample, upper_bound_sample;
    int i;
    unsigned approx_bytes_per_frame;
    uint32_t this_frame_sample = fc->samplenumber;
    unsigned this_block_size = fc->blocksize;
    bool needs_seek = true, first_seek = true;

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
    if(nseekpoints > 0) {
        /* Find the closest seek point <= target_sample, if it exists. */
        for(i = nseekpoints-1; i >= 0; i--) {
            if(seekpoints[i].sample <= target_sample)
                break;
        }
        if(i >= 0) { /* i.e. we found a suitable seek point... */
            lower_bound = fc->metadatalength + seekpoints[i].offset;
            lower_bound_sample = seekpoints[i].sample;
        }

        /* Find the closest seek point > target_sample, if it exists. */
        for(i = 0; i < nseekpoints; i++) {
            if(seekpoints[i].sample > target_sample)
                break;
        }
        if(i < nseekpoints) { /* i.e. we found a suitable seek point... */
            upper_bound = fc->metadatalength + seekpoints[i].offset;
            upper_bound_sample = seekpoints[i].sample;
        }
    }

    while(1) {
        /* Check if bounds are still ok. */
        if(lower_bound_sample >= upper_bound_sample ||
           lower_bound > upper_bound) {
            return false;
        }

        /* Calculate new seek position */
        if(needs_seek) {
            pos = (off_t)(lower_bound +
              (((target_sample - lower_bound_sample) *
              (int64_t)(upper_bound - lower_bound)) /
              (upper_bound_sample - lower_bound_sample)) -
              approx_bytes_per_frame);
            
            if(pos >= (off_t)upper_bound)
                pos = (off_t)upper_bound-1;
            if(pos < (off_t)lower_bound)
                pos = (off_t)lower_bound;
        }

        if(!ci->seek_buffer(pos))
            return false;

        bit_buffer = ci->request_buffer(&buff_size, MAX_FRAMESIZE+16);
        init_get_bits(&fc->gb, bit_buffer, buff_size*8);

        /* Now we need to get a frame.  It is possible for our seek
         * to land in the middle of audio data that looks exactly like
         * a frame header from a future version of an encoder.  When
         * that happens, frame_sync() will return false.
         * But there is a remote possibility that it is properly
         * synced at such a "future-codec frame", so to make sure,
         * we wait to see several "unparseable" errors in a row before
         * bailing out.
         */
        {
            unsigned unparseable_count;
            bool got_a_frame = false;
            for(unparseable_count = 0; !got_a_frame
                && unparseable_count < 30; unparseable_count++) {
                if(frame_sync(fc))
                    got_a_frame = true;
            }
            if(!got_a_frame) {
                ci->seek_buffer(orig_pos);
                return false;
            }
        }

        this_frame_sample = fc->samplenumber;
        this_block_size = fc->blocksize;

        if(target_sample >= this_frame_sample
           && target_sample < this_frame_sample+this_block_size) {
            /* Found the frame containing the target sample. */
            fc->sample_skip = target_sample - this_frame_sample;
            break;
        }

        if(this_frame_sample + this_block_size >= upper_bound_sample &&
           !first_seek) {
            if(pos == (off_t)lower_bound || !needs_seek) {
                ci->seek_buffer(orig_pos);
                return false;
            }
            /* Our last move backwards wasn't big enough, try again. */
            approx_bytes_per_frame *= 2;
            continue;
        }
        /* Allow one seek over upper bound,
         * required for streams with unknown total samples.
         */
        first_seek = false;

        /* Make sure we are not seeking in a corrupted stream */
        if(this_frame_sample < lower_bound_sample) {
            ci->seek_buffer(orig_pos);
            return false;
        }

        approx_bytes_per_frame = this_block_size*fc->channels*fc->bps/8 + 64;

        /* We need to narrow the search. */
        if(target_sample < this_frame_sample) {
            upper_bound_sample = this_frame_sample;
            upper_bound = ci->curpos;
        }
        else { /* Target is beyond this frame. */
            /* We are close, continue in decoding next frames. */
            if(target_sample < this_frame_sample + 4*this_block_size) {
                pos = ci->curpos + fc->framesize;
                needs_seek = false;
            }

            lower_bound_sample = this_frame_sample + this_block_size;
            lower_bound = ci->curpos + fc->framesize;
        }
    }

    return true;
}

/* Seek to file offset */
static bool flac_seek_offset(FLACContext* fc, uint32_t offset) {
    unsigned unparseable_count;
    bool got_a_frame = false;

    if(!ci->seek_buffer(offset))
        return false;

    bit_buffer = ci->request_buffer(&buff_size, MAX_FRAMESIZE);
    init_get_bits(&fc->gb, bit_buffer, buff_size*8);

    for(unparseable_count = 0; !got_a_frame
        && unparseable_count < 10; unparseable_count++) {
        if(frame_sync(fc))
            got_a_frame = true;
    }
    
    if(!got_a_frame) {
        ci->seek_buffer(fc->metadatalength);
        return false;
    }

    return true;
}

/* this is the codec entry point */
enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        /* Generic codec initialisation */
        ci->configure(DSP_SET_SAMPLE_DEPTH, FLAC_OUTPUT_DEPTH-1);
    }

    return CODEC_OK;
}

/* this is called for each file to process */
enum codec_status codec_run(void)
{
    int8_t *buf;
    uint32_t samplesdone;
    uint32_t elapsedtime;
    size_t bytesleft;
    int consumed;
    int res;
    int frame;
    intptr_t param;

    if (codec_init()) {
        LOGF("FLAC: Error initialising codec\n");
        return CODEC_ERROR;
    }

    /* Need to save resume for later use (cleared indirectly by flac_init) */
    elapsedtime = ci->id3->elapsed;
    samplesdone = ci->id3->offset;
    
    if (!flac_init(&fc,ci->id3->first_frame_offset)) {
        LOGF("FLAC: Error initialising codec\n");
        return CODEC_ERROR;
    }

    ci->configure(DSP_SET_FREQUENCY, ci->id3->frequency);
    ci->configure(DSP_SET_STEREO_MODE, fc.channels == 1 ?
                  STEREO_MONO : STEREO_NONINTERLEAVED);
    codec_set_replaygain(ci->id3);

    if (samplesdone || !elapsedtime) {
        flac_seek_offset(&fc, samplesdone);
        samplesdone=fc.samplenumber+fc.blocksize;
        elapsedtime=((uint64_t)samplesdone*1000)/(ci->id3->frequency);
    }
    else if (!flac_seek(&fc,(uint32_t)((uint64_t)elapsedtime
                            *ci->id3->frequency/1000))) {
        elapsedtime = 0;
    }

    ci->set_elapsed(elapsedtime);

    /* The main decoding loop */
    frame=0;
    buf = ci->request_buffer(&bytesleft, MAX_FRAMESIZE);
    while (bytesleft) {
        enum codec_command_action action = ci->get_command(&param);

        if (action == CODEC_ACTION_HALT)
            break;

        /* Deal with any pending seek requests */
        if (action == CODEC_ACTION_SEEK_TIME) {
            if (flac_seek(&fc,(uint32_t)(((uint64_t)param
                *ci->id3->frequency)/1000))) {
                /* Refill the input buffer */
                buf = ci->request_buffer(&bytesleft, MAX_FRAMESIZE);
            }

            ci->set_elapsed(param);
            ci->seek_complete();
        }

        if((res=flac_decode_frame(&fc,buf,
                             bytesleft,ci->yield)) < 0) {
             LOGF("FLAC: Frame %d, error %d\n",frame,res);
             return CODEC_ERROR;
        }
        consumed=fc.gb.index/8;
        frame++;

        ci->yield();
        ci->pcmbuf_insert(&fc.decoded[0][fc.sample_skip], &fc.decoded[1][fc.sample_skip],
                          fc.blocksize - fc.sample_skip);
        
        fc.sample_skip = 0;

        /* Update the elapsed-time indicator */
        samplesdone=fc.samplenumber+fc.blocksize;
        elapsedtime=((uint64_t)samplesdone*1000)/(ci->id3->frequency);
        ci->set_elapsed(elapsedtime);

        ci->advance_buffer(consumed);

        buf = ci->request_buffer(&bytesleft, MAX_FRAMESIZE);
    }

    LOGF("FLAC: Decoded %lu samples\n",(unsigned long)samplesdone);
    return CODEC_OK;
}
#endif

void flac_info(char *file, FLACContext *fc) {
    log_info("File %s: %d-bit %d-channel %d Hz max_block=%d data_start=%08X", file, fc->bps, fc->channels, fc->samplerate, fc->max_blocksize, fc->metadatalength);
}

void yield() { }

#define TEST 1
#undef TEST

int main(int argc, char **argv)
{
    int i, k, fd, phys_bps;
    void *mm, *mend, *mptr;
    void *pcmbuf; 
    size_t flen;
    FLACContext *fc;
    int consumed, scale;
    int32_t *src, *dst; 
    int16_t *dst16;   
#ifdef TEST
    int fd_out = -1;
    char *c;	
#endif    	

	if(argc < 2) {
	    log_err("FLAC decoder. Usage: %s <file.flac> [file.out]", argv[0]);
	    return -1;
	}
	fd = open(argv[1], O_RDONLY);
	if(fd < 0) {
	    log_err("cannot open %s", argv[1]);
	    return -1;	
	}
	flen = lseek(fd, 0, SEEK_END);
	if(flen < 0) {
	    log_err("lseek failed for %s", argv[1]);
	    close(fd);	
	    return -1;	
	}
	lseek(fd, 0, SEEK_SET);
	mm = mmap(0, flen, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);	
	if(mm == MAP_FAILED) {
	    log_err("mmap failed for %s", argv[1]);	
	    return -1;	
	}
	fc = flac_init(mm);
	if(!fc) {
	    munmap(mm, flen);	
	    return -1;
	}
	flac_info(argv[1],fc);

	fc->filesize = flen;
	fc->bitrate = ((int64_t) (fc->filesize - fc->metadatalength) * 8) / fc->length;

	phys_bps = (fc->bps == 24 ? 32 : fc->bps);
	
	/* to go */
	pcmbuf = malloc(fc->channels * (phys_bps/8) * MAX_BLOCKSIZE);
#ifdef TEST
	if(argc == 3) {
	    fd_out = open(argv[2], O_CREAT | O_RDWR | O_TRUNC, 0644);
	    if(fd_out < 0) {
		log_err("cannot open %s", argv[2]);
		munmap(mm, flen);
		flac_exit(fc);
		return -1;
	    }
	}
#endif
	mptr = mm + fc->metadatalength;
	mend = mm + flen;	
	scale = FLAC_OUTPUT_DEPTH - fc->bps;
	log_info("value at offset %08X: %08X", fc->metadatalength, *(uint32_t *)mptr);
	while(mptr < mend) {
	    k = flac_decode_frame(fc, mptr, MAX_FRAMESIZE/*mend - mptr*/, yield);
	    if(k < 0) {
		log_err("decoder error %d", k);
		break;
	    }		
	    consumed = fc->gb.index/8;
	    mptr += consumed;
/*	    log_info("decoded frame %d %d", fc->blocksize, consumed); */

	    switch(fc->bps) {
		case 32:
		    for(i = 0; i < fc->channels; i++) {
			src = fc->decoded[i];
			dst = (int32_t *) pcmbuf + i;
			for(k = 0; k < fc->blocksize; k++)  {
			    *dst = *src++;
			     dst += fc->channels;	
			}
		    }
		    break;	
#ifdef TEST
		case 24:	/* S24_3LE */
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
#else
		case 24:	/* S24_LE */
		    for(i = 0; i < fc->channels; i++) {
			src = fc->decoded[i];
			dst = (int32_t *) pcmbuf + i;
			for(k = 0; k < fc->blocksize; k++) {
			    *dst = (*src++ >> scale);
			     dst += fc->channels;
			}
		    }
		    break;
#endif
		case 16:		
		    for(i = 0; i < fc->channels; i++) {
			src = fc->decoded[i];
			dst16 = (int16_t *) pcmbuf + i;
		    	for(k = 0; k < fc->blocksize; k++) {
			    *dst16 = (int16_t) (*src++ >> scale);
			     dst16 += fc->channels;
			}
		    }
		    break;	
	    }
#ifdef TEST
	    if(fd_out >= 0) {
		k = fc->blocksize * fc->channels * (fc->bps/8);
		c = (char *) pcmbuf;
		while(k > 0) {	
		    i = TEMP_FAILURE_RETRY(write(fd_out, c, k));
		    if(i < 0) {
			log_err("write failed");
			munmap(mm, flen);
			flac_exit(fc);
			unlink(argv[2]);	
			return -1;
		    }
		    k -= i;
		    c += i;
		}
	    }
#endif
	}
#ifdef TEST
	if(fd_out >=0) close(fd_out);
#endif
	log_info("Complete.");
	flac_exit(fc);
	munmap(mm,flen);

    return 0;
}

