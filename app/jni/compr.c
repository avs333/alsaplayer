
#include "compr.h"

#ifdef _COMPR_PROTO_

int _FN(compr_fmt_check) (int fmt, uint64_t codecs_mask)
{
    switch(fmt) {
	case FORMAT_FLAC:	
	    if(codecs_mask & (1ULL << SND_AUDIOCODEC_FLAC)) return 0;
	    break;	
	case FORMAT_WAV:	
	    if(codecs_mask & (1ULL << SND_AUDIOCODEC_PCM)) return 0;
	    break;	
	case FORMAT_MP3:	
	    if(codecs_mask & (1ULL << SND_AUDIOCODEC_MP3)) return 0;
	    break;	
#if _COMPR_PROTO_  ==  0102
	case FORMAT_APE:	
	    if(codecs_mask & (1ULL << SND_AUDIOCODEC_APE)) return 0;
	    break;
#endif	
	default:
	    break;	
    }		
    return -1;	
}

/* -1 on error, MAX_NUM_CODECS otherwise  */
int _FN(compr_get_codecs) (int fd, int **ret_codecs)
{
     int i, *codecs = 0;
     struct snd_compr_caps caps;
	
	if(!ret_codecs) return -1;
	if(ioctl(fd, SNDRV_COMPRESS_GET_CAPS, &caps) != 0) {
	    log_err("cannot get compress capabilities for offload device");
	    return -1;	
	}
	codecs = (int *) malloc(sizeof(int) * MAX_NUM_CODECS);
	if(!codecs) {
	    log_err("no memory");
	    return -1;
	}
	memset(codecs, 0, sizeof(int) * MAX_NUM_CODECS);

	/* ignore this due to common msm kernel bug 
	log_info("total %d codecs supported", caps.num_codecs); */

	for(i = 0; i < MAX_NUM_CODECS; i++) codecs[i] = caps.codecs[i];

	*ret_codecs = codecs;	
	
    return MAX_NUM_CODECS;
}

int _FN(compr_get_caps) (int fd, int *min_fragments, 
	int *max_fragments, int *min_fragment_size, int *max_fragment_size) 
{
    struct snd_compr_caps caps;
    int ret;	 	    	
	if(!min_fragments || !max_fragments || !min_fragment_size 
		|| !max_fragment_size || fd < 0) return -1;	
	ret = ioctl(fd, SNDRV_COMPRESS_GET_CAPS, &caps);
	if(ret != 0) return ret;
        *min_fragment_size = caps.min_fragment_size;
	*max_fragment_size = caps.max_fragment_size;
	*min_fragments = caps.min_fragments;
	*max_fragments = caps.max_fragments;
    return 0;
}

int _FN(compr_set_hw_params) (playback_ctx *ctx, 
	int fd, int chunks, int chunk_size, int forced)
{
    struct snd_compr_params params;
    int k;

#if defined(ANDROID) || defined(ANDLINUX)
#define PROP_VALUE_MAX  92
    int __attribute__((weak)) __system_property_get(char *a, char *b);
    static int sdk = 0;
    char c[PROP_VALUE_MAX];
	if(!sdk) {
	    if(__system_property_get("ro.build.version.sdk",c) > 0) sscanf(c,"%d",&sdk);
	    else sdk = 16;	
	}
#endif

	k = alsa_get_rate(ctx->samplerate);
	if(k < 0) {
	    log_err("unsupported playback rate");
	    return LIBLOSSLESS_ERR_AU_SETUP;
	}
	memset(&params, 0, sizeof(params));
	params.codec.ch_in = ctx->channels;			
	params.codec.ch_out = ctx->channels;
#if defined(ANDROID) || defined(ANDLINUX)
	params.codec.sample_rate = (sdk >= 23) ? ctx->samplerate : k;
#else
	params.codec.sample_rate = k;
#endif
	log_info("source: format=%d, channels=%d, bps=%d, rate=%d bitrate=%d",
		ctx->file_format, ctx->channels, ctx->bps, ctx->samplerate, ctx->bitrate);

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
	    case FORMAT_WAV:
		params.codec.id = SND_AUDIOCODEC_PCM;
		params.codec.format = ctx->bps == 24 ? SNDRV_PCM_FORMAT_S24_LE : SNDRV_PCM_FORMAT_S16_LE;
		break;	
#if _COMPR_PROTO_  ==  0102
	    case FORMAT_APE:
		params.codec.id = SND_AUDIOCODEC_APE;
		params.codec.options.ape.compatible_version = ctx->ape_ver;
		params.codec.options.ape.compression_level = ctx->ape_compr;
		params.codec.options.ape.format_flags = ctx->ape_fmt;
		params.codec.options.ape.blocks_per_frame = ctx->ape_bpf;
		params.codec.options.ape.final_frame_blocks = ctx->ape_fin;
		params.codec.options.ape.total_frames = ctx->ape_tot;
		params.codec.options.ape.bits_per_sample = ctx->bps;
		params.codec.options.ape.num_channels = ctx->channels;
		params.codec.options.ape.sample_rate = ctx->samplerate;
		params.codec.options.ape.seek_table_present = 0;
		params.codec.format = ctx->bps == 24 ? SNDRV_PCM_FORMAT_S24_LE : SNDRV_PCM_FORMAT_S16_LE;
		break;
#endif
	    default:
		log_err("unsupported playback format %d", ctx->file_format);
		return LIBLOSSLESS_ERR_AU_SETUP;
	}

	params.buffer.fragments = chunks; 
	params.buffer.fragment_size = chunk_size;
/*	if(ctx->file_format == FORMAT_FLAC)
            log_info("setting params: codec=%d block_sz=%d/%d frm_sz=%d/%d smpl_sz=%d, %d chunks of size %d", 
		params.codec.id, params.codec.options.flac_dec.min_blk_size,
                params.codec.options.flac_dec.max_blk_size, params.codec.options.flac_dec.min_frame_size, 
		params.codec.options.flac_dec.max_frame_size, params.codec.options.flac_dec.sample_size,
		params.buffer.fragments, params.buffer.fragment_size); */

	while(1) {
	    errno = 0;
	    k = ioctl(fd, SNDRV_COMPRESS_SET_PARAMS, &params);
	    if(k == 0) break;	
#ifndef ANDROID
	    if(forced) {
                log_err("failed to set forced parameters for fragments num=%d size=%d (ret=%d)",
                        forced_chunks, forced_chunk_size, k);
		return  LIBLOSSLESS_ERR_AU_SETUP;
	    }	
#endif
	    chunks >>= 1;
	    if(!chunks) {
		log_err("failed to set hardware parameters");
		return LIBLOSSLESS_ERR_AU_SETUP;
	    }				
	    params.buffer.fragments = chunks;
	    log_info("hw params setup failed (err=%d, errno=%d) testing with %d chunks", k, errno, chunks); 
	}
    return 0;	
}

int _FN(compr_start_playback) (int fd)
{
    return ioctl(fd, SNDRV_COMPRESS_START);
}

int _FN(compr_drain) (int fd)
{
    return ioctl(fd, SNDRV_COMPRESS_DRAIN);
}

int _FN(compr_pause) (int fd)
{
    return ioctl(fd, SNDRV_COMPRESS_PAUSE, 1);
}

int _FN(compr_resume) (int fd)
{
    return ioctl(fd, SNDRV_COMPRESS_RESUME, 0);
}

int _FN(compr_offload_time_pos) (int fd)
{
    struct snd_compr_avail avail;
    int ret = ioctl(fd, SNDRV_COMPRESS_AVAIL, &avail); 
	if(ret != 0 || avail.tstamp.sampling_rate == 0) return 0;
    return avail.tstamp.pcm_io_frames / avail.tstamp.sampling_rate;
}

int _FN(compr_avail) (int fd, int *av)
{
    struct snd_compr_avail avail;
    int ret = ioctl(fd, SNDRV_COMPRESS_AVAIL, &avail); 
	if(ret != 0) return ret;
	if(av) *av = avail.avail;	
    return 0;
}

#else

#undef _COMPR_PROTO_
#define _COMPR_PROTO_   0101
#include "compr.h"

#undef _COMPR_PROTO_
#define _COMPR_PROTO_   0102
#include "compr.h"

int (*compr_fmt_check) (int fmt, uint64_t codecs_mask) = 0;
int (*compr_get_codecs) (int fd, int **codecs) = 0;
int (*compr_get_caps) (int fd, int *min_fragments, int *max_fragments, int *min_fragment_size, int *max_fragment_size) = 0;
int (*compr_set_hw_params) (playback_ctx *ctx, int fd, int chunks, int chunk_size, int force) = 0;
int (*compr_start_playback) (int fd) = 0;
int (*compr_drain) (int fd) = 0;
int (*compr_pause) (int fd) = 0;
int (*compr_resume) (int fd) = 0;
int (*compr_avail) (int fd, int *avail) = 0;
int (*compr_offload_time_pos) (int fd) = 0;

#define SNDRV_COMPRESS_IOCTL_VERSION    _IOR('C', 0x00, int)

#define SET_PTR(PTR, PROTO) PTR = &PTR ## _ ## PROTO

int compr_get_version(playback_ctx *ctx, int fd)
{
    int ret, version = 0;
	ret = ioctl(fd, SNDRV_COMPRESS_IOCTL_VERSION, &version);
	if(ret < 0) {
	    log_err("SNDRV_COMPRESS_IOCTL_VERSION failed!");
	    return -1;		
	}
	switch(version) {
	    case SNDRV_PROTOCOL_VERSION(0, 1, 1):
		log_info("switching to compress protocol version %08x", version);
		SET_PTR(compr_fmt_check, 0101);
		SET_PTR(compr_get_codecs, 0101);
		SET_PTR(compr_get_caps, 0101);
		SET_PTR(compr_set_hw_params, 0101);
		SET_PTR(compr_start_playback, 0101);
		SET_PTR(compr_drain, 0101);
		SET_PTR(compr_pause, 0101);
		SET_PTR(compr_resume, 0101);
		SET_PTR(compr_avail, 0101);
		SET_PTR(compr_offload_time_pos, 0101);
		break;
	    case SNDRV_PROTOCOL_VERSION(0, 1, 2):	
		log_info("switching to compress protocol version %08x", version);
		SET_PTR(compr_fmt_check, 0102);
		SET_PTR(compr_get_codecs, 0102);
		SET_PTR(compr_get_caps, 0102);
		SET_PTR(compr_set_hw_params, 0102);
		SET_PTR(compr_start_playback, 0102);
		SET_PTR(compr_drain, 0102);
		SET_PTR(compr_pause, 0102);
		SET_PTR(compr_resume, 0102);
		SET_PTR(compr_avail, 0102);
		SET_PTR(compr_offload_time_pos, 0102);
		break;	
	    default:
		log_err("unsupported compress protocol version %08x", version);
		return -1;
	}
    return 0;
}
#endif




