#ifndef ALSA_PRIV_INCLUDED
#define ALSA_PRIV_INCLUDED

#define MAX_RATES	8
#define MAX_FMTS	8

typedef struct _alsa_priv {
    /* per device params */	
    int card, device;
    char *card_name;
    char *devinfo;
    void *xml_dev;				/* device xml data handle */
    int is_offload;				/* compressed stream playback */
    int is_mmapped;				/* mmapped playback, not implemented yet */
    uint32_t supp_formats_mask;			/* as per mask field of supp_formats struct */
    uint32_t supp_rates_mask;			/* as per mask field of supp_rates struct */
    uint64_t supp_codecs_mask;			/* for offload playback */
    struct nvset *nv_start;			/* controls to start playback for this device */
    struct nvset *nv_stop;			/* controls to stop playback for this device */
    struct nvset *nv_rate[MAX_RATES];		/* controls to setup playback rates for this device */	
    struct nvset *nv_fmt[MAX_FMTS];		/* controls to setup playback formats for this device */
    struct nvset *nv_vol_analog[MAX_FMTS];	/* analog volume controls for this device */	
    struct nvset *nv_vol_digital[MAX_FMTS];	/* digital volume controls for this device */
    /* per track params */
    const playback_format_t *format;	
    int  chunks;				/* periods and period_size in frames for pcm playback, OR */
    int  chunk_size;				/* fragments and fragment_size in bytes for offload playback*/
    unsigned int  buffer_size;			/* chunk_size * chunks: for mmapped only */
    int  boundary;				/* for mmapped only */
    int  fd;					/* alsa device */
    struct snd_pcm_sync_ptr *sync_ptr;		/* for mmapped playback only */
    void *buf;					/* internal buffer for a single chunk OR complete mmapped buffer  */
    int  buf_bytes;				/* its size */
    int  cur_fmt;				/* index into nv_fmt[], for speedup */
    int  vol_analog[MAX_FMTS];			/* current analog/digital volumes; these are set */	
    int  vol_digital[MAX_FMTS];			/* to defaults when the device is switched */
} alsa_priv;

extern int alsa_get_rate(int rate);		/* SNDRV_PCM_RATE corresponding to numeric value */
extern int set_mixer_controls(playback_ctx *ctx, struct nvset *nv);

typedef enum { 
    VOL_SET_CURRENT, VOL_INCREASE, VOL_DECREASE 
} vol_ctl_t;

extern bool alsa_set_volume(playback_ctx *ctx, vol_ctl_t op);


#endif

