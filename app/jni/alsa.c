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
#include <pthread.h>
#include <limits.h>
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

struct ctl_elem {
    snd_ctl_elem_type_t type;
    char *name;
    int	 numid;
    int  count;
    int  evcount; 		/* count of possible values for enumerated ctl */
    char **evnames;		/* enumerated value names */
    struct ctl_elem *next;
};


static const playback_format_t supp_formats[] = {
    { SNDRV_PCM_FORMAT_S16_LE,  (1 << 0), 16, 16, "SNDRV_PCM_FORMAT_S16_LE" },
    { SNDRV_PCM_FORMAT_S24_LE,  (1 << 1), 32, 24, "SNDRV_PCM_FORMAT_S24_LE" },
    { SNDRV_PCM_FORMAT_S24_3LE, (1 << 2), 24, 24, "SNDRV_PCM_FORMAT_S24_3LE" },
    { SNDRV_PCM_FORMAT_S32_LE,  (1 << 3), 32, 32, "SNDRV_PCM_FORMAT_S32_LE" },
};
#define n_supp_formats (sizeof(supp_formats))/(sizeof(supp_formats[0]))

static const struct _supp_rates {
    int rate, mask;
} supp_rates[] = {
    { 44100, (1 << 0) }, { 88200, (1 << 1) }, { 176400, (1 << 2) },
    { 48000, (1 << 3) }, { 96000, (1 << 4) }, { 192000, (1 << 5) }
};
#define n_supp_rates (sizeof(supp_rates))/(sizeof(supp_rates[0]))

typedef struct _alsa_priv {
    /* life-cycle params */	
    void *xml_mixp;
    /* per device params */	
    int card, device;	
    char *card_name;
    void *xml_dev;
    int supp_formats_mask;
    int supp_rates_mask;	
    int setup_info;
    struct nvset *nv_start;
    struct nvset *nv_stop;
    struct nvset *nv_rate[n_supp_rates];	
    struct nvset *nv_fmt[n_supp_formats];	
    struct nvset *nv_vol_analog[n_supp_formats];
    struct nvset *nv_vol_digital[n_supp_formats];
    struct ctl_elem *ctls;	
    /* per track params */
    const playback_format_t *format;	
    int  periods, period_size;
    int  pcm_fd;
    void *pcm_buf;
    int  buf_bytes;
    int  cur_fmt;	/* index into nv_fmt[] */
    int  cur_rate;	/* index into nv_rate[] */
    /* Set to defaults when the card/device is switched */	
    int  vol_analog[n_supp_formats];
    int  vol_digital[n_supp_formats];
} alsa_priv;

static void free_mixer_controls(playback_ctx *ctx);
static int init_mixer_controls(playback_ctx *ctx, int card);
static int set_mixer_controls(playback_ctx *ctx, struct nvset *nv);

typedef enum { VOL_SET_CURRENT, VOL_INCREASE, VOL_DECREASE } vol_ctl_t;
static bool alsa_set_volume(playback_ctx *ctx, vol_ctl_t op); 

#if defined(ANDROID) || defined(ANDLINUX)
char mixer_paths_file[] = "/system/etc/mixer_paths.xml";
char cards_file[] = "/sdcard/.alsaplayer/cards.xml";
#else
char mixer_paths_file[PATH_MAX];
char cards_file[PATH_MAX];
#endif

static void alsa_close(playback_ctx *ctx) 
{
    alsa_priv *priv;	
	if(!ctx) return;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) return;		
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	if(priv->pcm_buf) free(priv->pcm_buf);
	priv->pcm_fd = -1;	
	priv->pcm_buf = 0;
}

void alsa_exit(playback_ctx *ctx)
{
    alsa_priv *priv;
    int k;    	
	if(!ctx) return;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) return;
	alsa_close(ctx);
	free_mixer_controls(ctx);
	if(priv->xml_mixp) xml_mixp_close(priv->xml_mixp);
	if(priv->xml_dev) xml_dev_close(priv->xml_dev);
	if(priv->card_name) free(priv->card_name);
	if(priv->nv_start) free_nvset(priv->nv_start);
	if(priv->nv_stop) free_nvset(priv->nv_stop);
	for(k = 0; k < n_supp_rates; k++) 
	    if(priv->nv_rate[k]) free(priv->nv_rate[k]);
	for(k = 0; k < n_supp_formats; k++) { 
	    if(priv->nv_fmt[k]) free(priv->nv_fmt[k]);
	    if(priv->nv_vol_digital[k]) free(priv->nv_vol_digital[k]);
	    if(priv->nv_vol_analog[k]) free(priv->nv_vol_analog[k]);
	}
	free(priv);
	ctx->alsa_priv = 0;
}


#define param_to_interval(p,n)	(&(p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]))
#define param_to_mask(p,n)	(&(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]))

static inline void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
    struct snd_mask *m = param_to_mask(p, n);
	m->bits[0] = 0;
	m->bits[1] = 0;
	m->bits[bit >> 5] |= (1 << (bit & 31));
}

static inline void param_set_int(struct snd_pcm_hw_params *p, int n, unsigned int val)
{
    struct snd_interval *i = param_to_interval(p, n);
	i->min = val;
	i->max = val;
	i->integer = 1;
}

static inline void param_set_range(struct snd_pcm_hw_params *p, int n, unsigned int min, unsigned int max)
{
    struct snd_interval *i = param_to_interval(p, n);
	i->min = min;
	i->max = max;
	i->integer = 1;
}

static void param_init(struct snd_pcm_hw_params *p)
{
    int n;
    memset(p, 0, sizeof(*p));
    for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK;
         n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
            struct snd_mask *m = param_to_mask(p, n);
            m->bits[0] = ~0;
            m->bits[1] = ~0;
    }
    for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
         n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
            struct snd_interval *i = param_to_interval(p, n);
            i->min = 0;
            i->max = ~0;
    }
    p->rmask = ~0U;
    p->cmask = 0;
    p->info = ~0U;
}

static inline void setup_hwparams(struct snd_pcm_hw_params *params, 
	int fmt, int rate, int channels, int periods, int period_size) 
{
    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    if(fmt) param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, fmt);		/* we don't support U8 = 0 */
    if(rate) param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, rate);
    if(channels) param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS, channels);	
    if(period_size) param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_size);
    if(periods) param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, periods);
}

int alsa_select_device(playback_ctx *ctx, int card, int device) 
{
    char tmp[128];
    int  k, fd = -1, ret = 0;
    struct snd_pcm_hw_params hwparams;
    alsa_priv *priv;
    struct snd_ctl_card_info info;

	if(!ctx) {
	    log_err("no context");
	    return LIBLOSSLESS_ERR_NOCTX;
	}
	if(card < 0 || device < 0) {
	    log_err("invalid card/device=%d/%d", card, device);	
	    return LIBLOSSLESS_ERR_INV_PARM;
	}
	priv = (alsa_priv *) ctx->alsa_priv;
	if(priv) {
	    if(priv->card == card && priv->device == device) {
		log_info("card/device unchanged");
		return 0;
	    } else {
		alsa_close(ctx);
		alsa_exit(ctx);	
		log_info("switching to card %d device %d", card, device);
	    }
	}

	ctx->alsa_priv = calloc(1, sizeof(alsa_priv));
	if(!ctx->alsa_priv) return LIBLOSSLESS_ERR_NOMEM;

	priv = (alsa_priv *) ctx->alsa_priv;	

	priv->card = card;	
	priv->device = device;

	if(init_mixer_controls(ctx, card) != 0) {
	    log_err("cannot open mixer for card %d", card);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
#if !defined(ANDROID) && !defined(ANDLINUX)
	sprintf(mixer_paths_file, "%s/.alsaplayer/mixer_paths.xml", getenv("HOME"));	
	sprintf(cards_file, "%s/.alsaplayer/cards.xml", getenv("HOME"));	
#endif

	priv->xml_mixp = xml_mixp_open(mixer_paths_file);
	if(!priv->xml_mixp) log_info("%s missing", mixer_paths_file);
	else log_info("%s opened", mixer_paths_file);

	sprintf(tmp, "/dev/snd/controlC%d", card);
	fd = open(tmp, O_RDWR);
	if(fd < 0) { /* cannot happen as init_mixer_controls has succeeded */
	    log_err("cannot open mixer");
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
	if(ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) != 0 || !info.name) {
	    log_err("card info query failed");
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
	priv->card_name = strdup((char *)info.name);
	close(fd); 
	fd = -1;

	priv->xml_dev = xml_dev_open(cards_file, priv->card_name, device);	
	if(!priv->xml_dev) log_info("warning: no settings for card %d (%s) device %d in %s", card, priv->card_name, device, cards_file); 
	else {
	    struct nvset *nv1 = 0, *nv2 = 0, *nv = 0; 
	    int hset = 0;
	    log_info("loaded settings for card %d device %d", card, device);
	    if(priv->xml_mixp && xml_dev_is_builtin(priv->xml_dev)) {
		priv->nv_start = xml_mixp_find_control_set(priv->xml_mixp, "headphones");
		if(!priv->nv_start) {
		    priv->nv_start = xml_mixp_find_control_set(priv->xml_mixp, "headset");
		    hset = 1;	
		}
		if(priv->nv_start) {
		    log_info("start/stop controls found");	
		    for(nv1 = priv->nv_start; nv1->next; nv1 = nv1->next) ;	/* set nv1 -> tail of nv_start */
		    priv->nv_stop = xml_mixp_find_control_set(priv->xml_mixp, hset ? "headset" : "headphones"); /* the same again */
		    for(nv2 = priv->nv_stop; nv2->next; nv2 = nv2->next) 	/* reset to default values & set nv2 -> tail */
		    	nv2->value = xml_mixp_find_control_default(priv->xml_mixp, nv2->name);
		    nv2->value = xml_mixp_find_control_default(priv->xml_mixp, nv2->name); 	/* last one */
		} else log_info("no headphones/headset path");
	    } else log_info("no headphones/headset path");

	    nv = xml_dev_find_ctls(priv->xml_dev, "start", 0);
	    if(!priv->nv_start) priv->nv_start = nv;
	    else nv1->next = nv;	/* if nv_start was found before, nv1 must point to its tail */

	    nv = xml_dev_find_ctls(priv->xml_dev, "stop", 0);
	    if(!priv->nv_stop) priv->nv_stop = nv2 = nv;
	    else nv2->next = nv; 	/* if nv_stop was found before, nv2 must point to its tail */

	    while(nv2 && nv2->next) nv2 = nv2->next;	/* set nv2 -> tail of nv_stop */
	    
	    /* add rate/fmt default values to nv_stop: mixer_paths does not include them! */
	    nv = xml_dev_find_ctls(priv->xml_dev, "rate", "default");
	    if(nv) log_info("will use %s as default rate", nv->value);
	    else log_info("no default rate");	

	    if(!priv->nv_stop) priv->nv_stop = nv2 = nv;
	    else nv2->next = nv;

	    while(nv2 && nv2->next) nv2 = nv2->next;	/* set nv2 -> tail of nv_stop */

	    nv = xml_dev_find_ctls(priv->xml_dev, "fmt", "default");
	    if(nv) log_info("will use %s as default format", nv->value);
	    else log_info("no default format");	

	    if(!priv->nv_stop) priv->nv_stop = nv;
	    else nv2->next = nv;	/* if nv_stop was found before, nv2 must point to its tail */	
	}
	/* fire up */
	if(priv->nv_start) {
	    k = set_mixer_controls(ctx, priv->nv_start);
	    if(k < 0) {
		log_err("failed to startup card %d", card);
		ret = LIBLOSSLESS_ERR_AU_SETUP;
		goto err_exit;
	    }
	}
	sprintf(tmp, "/dev/snd/pcmC%dD%dp", card, device);
	fd = open(tmp, O_RDWR);
	if(fd < 0) {
	    log_err("cannot open card %d device %d", card, device);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;	
	    goto err_exit;	
	}
	for(k = 0; k < n_supp_formats; k++) {	
	    setup_hwparams(&hwparams, supp_formats[k].fmt, 0, 0, 0, 0);
	    if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hwparams) == 0) {
		priv->supp_formats_mask |= supp_formats[k].mask;
		if(!priv->xml_dev) continue;
		priv->nv_fmt[k] = xml_dev_find_ctls(priv->xml_dev, "fmt", supp_formats[k].str);
		if(priv->nv_fmt[k]) log_info("found controls for fmt=%s", supp_formats[k].str);
		else log_info("will use defaults for fmt=%s", supp_formats[k].str);
	    	priv->nv_vol_analog[k] = xml_dev_find_ctls(priv->xml_dev, "analog_volume", supp_formats[k].str);
		if(!priv->nv_vol_analog[k]) priv->nv_vol_analog[k] = xml_dev_find_ctls(priv->xml_dev, "analog_volume", 0);
		if(priv->nv_vol_analog[k]) {
		    priv->vol_analog[k] = atoi(priv->nv_vol_analog[k]->value);	/* set to default analog value */
		    log_info("found analog volume controls for fmt=%s: %s %d/%d/%d", supp_formats[k].str, 
			priv->nv_vol_analog[k]->name, priv->vol_analog[k], priv->nv_vol_analog[k]->min, priv->nv_vol_analog[k]->max);
		}
	    	priv->nv_vol_digital[k] = xml_dev_find_ctls(priv->xml_dev, "digital_volume", supp_formats[k].str);
		if(!priv->nv_vol_digital[k]) priv->nv_vol_digital[k] = xml_dev_find_ctls(priv->xml_dev, "digital_volume", 0);
		if(priv->nv_vol_digital[k]) {
		    priv->vol_digital[k] = atoi(priv->nv_vol_digital[k]->value); /* set to default digital value */
		    log_info("found digital volume controls for fmt=%s: %s %d/%d/%d", supp_formats[k].str,
			priv->nv_vol_digital[k]->name, priv->vol_digital[k], priv->nv_vol_digital[k]->min, priv->nv_vol_digital[k]->max);
		}
	    }
	}
	for(k = 0; k < n_supp_rates; k++) {
	    int rate = supp_rates[k].rate;	
	    setup_hwparams(&hwparams, 0, rate, 0, 0, 0);	
	    if(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hwparams) == 0) {
		priv->supp_rates_mask |= supp_rates[k].mask;
		if(priv->xml_dev) {
		    sprintf(tmp, "%d", rate);
		    priv->nv_rate[k] = xml_dev_find_ctls(priv->xml_dev, "rate", tmp);
		    if(priv->nv_rate[k]) log_info("found controls for rate=%d", rate);
		    else log_info("will use defaults for rate=%d", rate);
		}
	    }
	}
	if(!priv->supp_rates_mask || !priv->supp_formats_mask) {
	    log_err("unsupported hardware (format/rate masks=0x%x/0x%x)", 
		priv->supp_formats_mask, priv->supp_rates_mask);
	    ret = LIBLOSSLESS_ERR_AU_GETCONF;
	    goto err_exit;	
	}
	close(fd);
	log_info("selected card=%d (%s) device=%d (format/rate masks=0x%x/0x%x)", 
		card, priv->card_name, device, priv->supp_formats_mask, priv->supp_rates_mask);

	return ret;

    err_exit:
	if(fd >= 0) close(fd);
	alsa_exit(ctx);
    return ret;	

}

int alsa_start(playback_ctx *ctx) 
{
    char tmp[128];
    struct snd_pcm_hw_params hwparams, *params = &hwparams;
    struct snd_pcm_sw_params swparams;
    int i, k, ret = 0;
    int periods_min, periods_max, persz_min, persz_max;
    alsa_priv *priv;

	if(!ctx || !ctx->alsa_priv) return LIBLOSSLESS_ERR_INV_PARM;
	priv = (alsa_priv *) ctx->alsa_priv;

	for(i = 0; i < n_supp_formats; i++)
	    if(supp_formats[i].strm_bits == ctx->bps && 
		(supp_formats[i].mask & priv->supp_formats_mask)) {
		priv->cur_fmt = i;
		if(priv->nv_fmt[i]) set_mixer_controls(ctx, priv->nv_fmt[i]);
		break;
	    }
	if(i == n_supp_formats) {
	    log_err("device does not support %d-bit files", ctx->bps);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;		
	}
	priv->format = &supp_formats[i];
	ctx->rate_dec = 0;

	for(k = 0, ret = 1; k <= 2 && ret; k++) {
	    for(i = 0; i < n_supp_rates; i++) {
		if(supp_rates[i].rate == (ctx->samplerate >> k) &&
		    (supp_rates[i].mask & priv->supp_rates_mask)) {
		    priv->cur_rate = i;
		    if(priv->nv_rate[i]) set_mixer_controls(ctx, priv->nv_rate[i]);
		    if(k) {
			log_info("WARNING: rate=%d not supported by hardware, will be downsampled to %d", 
				ctx->samplerate, ctx->samplerate >> k);	
			ctx->samplerate >>= k; 	
			ctx->rate_dec = k;
		    }
		    ret = 0;
		    break;
		}
	    }
	}

	if(ret) {
	    log_err("samplerate %d not supported", ctx->samplerate);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}

	if(priv->nv_start) set_mixer_controls(ctx, priv->nv_start);
	else log_info("no start controls for this device");

	alsa_set_volume(ctx, VOL_SET_CURRENT);	/* must reset according to cur_fmt */

	log_info("opening pcm");
	sprintf(tmp, "/dev/snd/pcmC%dD%dp", priv->card, priv->device);
	priv->pcm_fd = open(tmp, O_RDWR);
	if(priv->pcm_fd < 0) {
	    log_err("cannot open %s", tmp);
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;
	}
	log_info("pcm opened");

	setup_hwparams(params, priv->format->fmt, ctx->samplerate, ctx->channels, 0, 0);

 	log_info("Trying: format=%s rate=%d channels=%d bps=%d (phys=%d)", priv->format->str, 
	    ctx->samplerate, ctx->channels, ctx->bps, priv->format->phys_bits);	

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, params) != 0) {
	    log_info("refine failed");
	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
	/* sanity check */
	i = param_to_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->max;
	if(i != priv->format->phys_bits ||
		i != param_to_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min) {
	    log_info("bogie refine");
     	    ret = LIBLOSSLESS_ERR_AU_SETUP;
	    goto err_exit;	
	}
	periods_max = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->max;
	periods_min = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIODS)->min;
	persz_max = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
	persz_min = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;

	log_info("Period size: min=%d\tmax=%d", persz_min, persz_max);
	log_info("    Periods: min=%d\tmax=%d", periods_min, periods_max);

	priv->periods = periods_max;
	priv->period_size = persz_max;

	param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, priv->period_size);
	param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, priv->periods);

	/* Try to obtain the largest buffer possible keeping in mind 
	   that ALSA always tries to set minimum latency */

	if(priv->periods > 32) priv->periods = 32; 

#define NSTEPS	8

	i = (persz_max - persz_min) / NSTEPS; 

	while(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, params) < 0) {
	    priv->period_size -= i;
	    if(priv->period_size < persz_min || i == 0) {
		priv->period_size = persz_max;
		priv->periods >>= 1;	
	    } else if(priv->period_size - i < persz_min && (i/NSTEPS) > 0) { /* Refine last step */
		log_info("refine");
		priv->period_size += i; /* undo */	
		i /= NSTEPS;
		priv->period_size -= i;		
	    }
	    if(priv->periods < periods_min) {
		log_err("cannot set hw parameters");
		ret = LIBLOSSLESS_ERR_AU_SETCONF;
		goto err_exit;
	    }
	    setup_hwparams(params, priv->format->fmt, ctx->samplerate, ctx->channels, priv->periods, 0);
	    param_set_range(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, priv->period_size, persz_max);		
	    log_info("retrying with period_size %d periods %d", priv->period_size, priv->periods);	
	}
	priv->period_size = param_to_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
	priv->setup_info = params->info;

	log_info("selecting period size %d, periods %d [hw_info=0x%08X]",
		priv->period_size, priv->periods, priv->setup_info);

	priv->buf_bytes = priv->period_size * ctx->channels * priv->format->phys_bits/8;
	priv->pcm_buf = malloc(priv->buf_bytes);
	if(!priv->pcm_buf) {
	    log_err("no memory for buffer");
	    ret = LIBLOSSLESS_ERR_NOMEM;
	    goto err_exit;	
	}
        memset(priv->pcm_buf, 0, priv->buf_bytes);

	memset(&swparams, 0, sizeof(swparams));

	swparams.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
	swparams.period_step = 1;

#if 0
	swparams.avail_min = priv->period_size; 	/* by default */
#else
	/* wake up as soon as possible */
	swparams.avail_min = 1;
#endif
	swparams.start_threshold = priv->period_size;

	swparams.boundary = priv->period_size * priv->periods;

/* PCM is automatically stopped in SND_PCM_STATE_XRUN state when available frames is >= threshold. 
  If the stop threshold is equal to boundary (also software parameter - sw_param) then automatic stop will be disabled 
  (thus device will do the endless loop in the ring buffer). */
#if 0
	swparams.stop_threshold = priv->period_size * (priv->periods - 1); 
#else
	swparams.stop_threshold = swparams.boundary;
#endif

/* A portion of playback buffer is overwritten with silence when playback underrun is nearer than silence threshold.
The special case is when silence size value is equal or greater than boundary. The unused portion of the ring buffer 
(initial written samples are untouched) is filled with silence at start. Later, only just processed sample area is 
filled with silence. Note: silence_threshold must be set to zero.  */
#if 0
	/* error on codeaurora */
	swparams.silence_size = swparams.boundary;
	swparams.silence_threshold = 0;
#else
	swparams.silence_size = 0;
	swparams.silence_threshold = 0;
#endif

	/* obsolete: xfer size need to be a multiple (of whatever) */
/*	swparams.xfer_align = priv->period_size / 2; */
	swparams.xfer_align = 1;

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_SW_PARAMS, &swparams) < 0) {
	    log_err("falied to set sw parameters");
	    ret = LIBLOSSLESS_ERR_AU_SETCONF;
	    goto err_exit;
	}

	if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            log_err("prepare() failed");
            ret = LIBLOSSLESS_ERR_AU_SETCONF;
            goto err_exit;
        }
#if 0
	for(k = 0; k < priv->periods; k++) {
	    i = write(priv->pcm_fd, priv->pcm_buf, priv->buf_bytes);
	    if(i != priv->buf_bytes) {
		log_err("cannot fill initial buffer");
		ret = LIBLOSSLESS_ERR_AU_BUFF;
		goto err_exit;
	    }
	}
#endif
	log_info("setup complete");
	return 0;

    err_exit:	
	if(priv->nv_stop) set_mixer_controls(ctx, priv->nv_stop);
	if(priv->pcm_buf) {
	    free(priv->pcm_buf);
	    priv->pcm_buf = 0;	
	    priv->buf_bytes = 0;
	}
	if(priv->pcm_fd >= 0) close(priv->pcm_fd);
	priv->pcm_fd = -1;
	log_err("exiting on error");

    return ret;
}

ssize_t alsa_write(playback_ctx *ctx, size_t count)
{
    alsa_priv *priv;
    int i, written = 0;
    struct snd_xferi xf;
    struct snd_pcm_status pcm_stat;

	if(!ctx || !ctx->alsa_priv || ctx->state != STATE_PLAYING) {
	    log_err("stream must be closed or paused");	
	    return 0;
	}
	priv = (alsa_priv *) ctx->alsa_priv;	
	
	if(count > priv->period_size) {
	    log_err("frames count %d larger than period size %d", (int) count, priv->period_size);
	    count = priv->period_size;
	} else if(count < priv->period_size) {
	    log_info("short buffer, must be EOF");	
	    memset(priv->pcm_buf + count * ctx->channels * priv->format->phys_bits/8, 0, 
			(priv->period_size - count) * ctx->channels * priv->format->phys_bits/8);
	}	

	xf.buf = priv->pcm_buf;
	xf.frames = priv->period_size;
	xf.result = 0;

	while(written < priv->period_size) {
#if 1
	    i = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_STATUS, &pcm_stat);
	    if(i) {
		log_err("failed to obtain pcm status");
		ctx->alsa_error = 1;
		return 0;	
	    } /* else log_info("hw/app=%ld/%ld avail/max=%ld/%ld", pcm_stat.hw_ptr, pcm_stat.appl_ptr, pcm_stat.avail, pcm_stat.avail_max); */
#endif
	    i = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xf);
#if 0
	    if(!i && ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_STATUS, &pcm_stat) == 0 && pcm_stat.hw_ptr == pcm_stat.appl_ptr) {
		log_info("underrun to occur: %ld %ld %ld %ld", pcm_stat.hw_ptr, pcm_stat.appl_ptr, pcm_stat.avail, pcm_stat.avail_max);
		if(pcm_stat.avail > xf.frames) {
		    i = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_FORWARD, pcm_stat.avail - xf.frames);
		    log_info("ioctl(SNDRV_PCM_IOCTL_FORWARD,%d)=%d", pcm_stat.avail - xf.frames, i);
		}
	    } 	
#endif
	    if(i != 0) {
		switch(errno) {
		   case EINTR:
			log_info("exiting on EINTR");
			break;
		   case EAGAIN:
			log_err("EAGAIN");
			usleep(1000);
			break;
		   case EPIPE:
			log_info("underrun!");
			ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_STATUS, &pcm_stat);
			log_info("%ld %ld %ld %ld", pcm_stat.hw_ptr, pcm_stat.appl_ptr, pcm_stat.avail, pcm_stat.avail_max);
			if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
			    log_err("prepare failed after underrun");
			    ctx->alsa_error = 1;
			    return 0;	
			}
			break;
		   default:
			log_info("exiting on %s (%d)", strerror(errno), errno);
			ctx->alsa_error = 1;
			return 0;		
		}	
	    }
	    written += xf.result;
	}	
	ctx->written += count;

    return written;

#if 0
	for(k = 0, written = 0; k < priv->periods; k++) {
	    while(written < count) {
		i = write(priv->pcm_fd, priv->pcm_buf + k * priv->period_size * ctx->channels * priv->format->phys_bits/8,
			priv->period_size * ctx->channels * priv->format->phys_bits/8);
		if(i != priv->period_size * ctx->channels * priv->format->phys_bits/8) {
		    switch(errno) {
			case EINTR:
			case EAGAIN:
			    log_info("ioctl error: %s", strerror(errno));
			    usleep(100);
			    break;
			case EPIPE:
			    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
				log_err("prepare failed after underrun");
				return 0;	
			    }
			    log_info("underrun occurred");
			    break;
			default:
			    log_info("unhandled error %d in WRITEI_FRAMES: %s (%d)", i, strerror(errno), errno);
			    return 0;		
		    }
		} else written += priv->period_size;
	    }
	}
#endif
}

void *alsa_get_buffer(playback_ctx *ctx)
{
    alsa_priv *priv;	
	if(!ctx) return 0;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) return 0;
    return priv->pcm_buf;	
}

int alsa_get_period_size(playback_ctx *ctx)
{
    alsa_priv *priv;	
	if(!ctx) return 0;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) return 0;
    return priv->period_size; 	
}

const playback_format_t *alsa_get_format(playback_ctx *ctx)
{
    alsa_priv *priv;	
	if(!ctx) return 0;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(!priv) return 0;
    return priv->format;  	
}


void alsa_stop(playback_ctx *ctx) 
{
    alsa_priv *priv;	

	if(!ctx || !ctx->alsa_priv) return;
	priv = (alsa_priv *) ctx->alsa_priv;
	if(priv->pcm_fd >= 0) {
	    log_info("closing pcm stream");	
#if 0
	    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_DRAIN) == 0) log_info("pcm_drain: success");	
	    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_DROP) == 0) log_info("pcm_drop: success");	
	    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_RESET) == 0) log_info("pcm_reset: success");	
	    if(ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_XRUN) == 0) log_info("pcm_xrun: success");	
	    { 
		int flags;
		    if((flags = fcntl(priv->pcm_fd, F_GETFL)) != -1) {
			flags |= O_NONBLOCK;
			fcntl(priv->pcm_fd, F_SETFL, flags);
		    }
	    }		
#endif
	    close(priv->pcm_fd);
	    log_info("pcm stream closed");
	}
	priv->pcm_fd = -1;
	if(priv->nv_stop) set_mixer_controls(ctx, priv->nv_stop);	
	if(priv->pcm_buf) free(priv->pcm_buf);
	priv->pcm_buf = 0;
}

#if 0
static bool alsa_pause_ioctl(playback_ctx *ctx, int push) 
{
    int ret;
    alsa_priv *priv;
	if(!ctx || !ctx->alsa_priv) return false;
	priv = (alsa_priv *) ctx->alsa_priv;
	if((priv->setup_info & SNDRV_PCM_INFO_PAUSE) == 0) {
	    log_err("pause/resume not supported by hardware");
	    return false;
	}
	ret = ioctl(priv->pcm_fd, SNDRV_PCM_IOCTL_PAUSE, push);
    return ret == 0;
}
#endif

bool alsa_pause(playback_ctx *ctx) 
{
    alsa_stop(ctx);	
    return true;	
}

bool alsa_resume(playback_ctx *ctx) 
{
   return alsa_start(ctx) == 0;
}

static bool alsa_set_volume(playback_ctx *ctx, vol_ctl_t op) 
{
    alsa_priv *priv;
    char tmp[128];
    struct nvset *nvd, *nva, *nv;
    int  vola, vold;
   	
	if(!ctx || !ctx->alsa_priv) return false;	
	priv = (alsa_priv *) ctx->alsa_priv;
	nvd = priv->nv_vol_digital[priv->cur_fmt];
	nva = priv->nv_vol_analog[priv->cur_fmt];
	if(!nvd && !nva) {
	    log_err("don't know how to control volume of this card");
	    return false;
	}	
	if(priv->ctls == 0)  {
	    log_err("no mixer");
	    return false;	
	}

	vold = priv->vol_digital[priv->cur_fmt];
	vola = priv->vol_analog[priv->cur_fmt];

#define VOL_STEP	5	/* decrease/increase in percentage */

	switch(op) {
	    case VOL_SET_CURRENT:
		log_info("VOL_SET_CURRENT");
		break;	
	    case VOL_INCREASE:
		if(nvd && nvd->max > nvd->min) {
		    vold += (VOL_STEP * (nvd->max - nvd->min))/100;
		    if(vold > nvd->max) vold = nvd->max;
		}
		if(nva && nva->max > nva->min) {
		    vola += (VOL_STEP * (nva->max - nva->min))/100;
		    if(vola > nva->max) vola = nva->max;
		}
		break;
	    case VOL_DECREASE:
		if(nvd && nvd->max > nvd->min) {
		    vold -= (VOL_STEP * (nvd->max - nvd->min))/100;
		    if(vold < nvd->min) vold = nvd->min;
		}
		if(nva && nva->max > nva->min) {
		    vola -= (VOL_STEP * (nva->max - nva->min))/100;
		    if(vola < nva->min) vola = nva->min;
		}
		break;
	    default:
		log_err("invalid op %d specified", op);
		return false;
	}
	if(nvd && (op == VOL_SET_CURRENT || priv->vol_digital[priv->cur_fmt] != vold)) {
	    sprintf(tmp, "%d", vold);
	    for(nv = nvd; nv; nv = nv->next) nv->value = tmp; 
	    if(set_mixer_controls(ctx, nvd)) priv->vol_digital[priv->cur_fmt] = vold;
	}
	if(nva && (op == VOL_SET_CURRENT || priv->vol_analog[priv->cur_fmt] != vola)) {
	    sprintf(tmp, "%d", vola);
	    for(nv = nva; nv; nv = nv->next) nv->value = tmp; 
	    if(set_mixer_controls(ctx, nva)) priv->vol_analog[priv->cur_fmt] = vola;
	}
     return true; 
}

bool alsa_increase_volume(playback_ctx *ctx) 
{
    return alsa_set_volume(ctx, VOL_INCREASE);	    	
}

bool alsa_decrease_volume(playback_ctx *ctx) 
{
    return alsa_set_volume(ctx, VOL_DECREASE);	    	
}

/********************************************/
/************** MIXER STUFF *****************/
/********************************************/

static int init_mixer_controls(playback_ctx *ctx, int card)
{
    int i,k;
    char tmp[256];		
    struct snd_ctl_elem_list elist;
    struct snd_ctl_elem_id *eid = 0;

    struct snd_ctl_elem_info ei;
    struct ctl_elem *ctl = 0;
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
    int ctl_fd = -1;

	if(card < 0) {
	    log_err("invalid card=%d", card);	   
	    return -1;
	}
	if(priv->ctls) free_mixer_controls(ctx);

	snprintf(tmp, sizeof(tmp), "/dev/snd/controlC%d", card);
	ctl_fd = open(tmp, O_RDWR);	
	if(ctl_fd < 0) {
	    log_err("cannot open mixer");
	    return -1;
	}
	memset(&elist, 0, sizeof(elist));

	if(ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
	    log_err("cannot get number of controls");
	    goto err_exit;	
	}
	eid = calloc(elist.count, sizeof(struct snd_ctl_elem_id));
	if(!eid) {
	    log_err("no memory");
	    goto err_exit;	
	}
	elist.space = elist.count;
	elist.pids = eid;
	if(ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
	    log_err("cannot get control ids");
	    goto err_exit; 	
	}
	for(k = 0; k < elist.count; k++) {
		 /* get info for each control id */
	    memset(&ei, 0, sizeof(ei));
	    ei.id.numid = eid[k].numid;
	    if(ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) < 0) {
		    log_err("cannot get info for control id %d\n", ei.id.numid); 
		    continue;
	    }
	    if(!priv->ctls) {
		priv->ctls = (struct ctl_elem *) calloc(1, sizeof(struct ctl_elem));
		ctl = priv->ctls;
	    } else {
		ctl->next = (struct ctl_elem *) calloc(1, sizeof(struct ctl_elem));;
		ctl = ctl->next;
	    }
	    if(!ctl) {
		log_err("no memory");
		goto err_exit;
	    }	
	    ctl->type = ei.type;
	    ctl->numid = ei.id.numid;
	    ctl->name = strdup((char *)ei.id.name);
	    ctl->count = ei.count;
	    if(ctl->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		ctl->evcount = ei.value.enumerated.items;
		ctl->evnames = (char **) malloc(sizeof(char *) * ctl->evcount);
		if(!ctl->evnames) {
		    log_err("no memory");
		    goto err_exit;
		}
		for(i = 0; i < ctl->evcount; i++) {
		    ei.value.enumerated.item = i;
		    if(ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) < 0) {
			log_err("cannot get name[%d] of enum control %s\n", i, ctl->name);
                                break;
		    }
		    ctl->evnames[i] = strdup(ei.value.enumerated.name);		
		}
	    }
	} 
	close(ctl_fd);
	return 0;

    err_exit:
	free_mixer_controls(ctx);
	if(eid) free(eid);
	if(ctl_fd >= 0) close(ctl_fd);
    return -1;	
}

/* returns the number of controls set successfully */
static int set_mixer_controls(playback_ctx *ctx, struct nvset *nv)
{
    int i, k, n = 0;
    struct ctl_elem *ctl;
    struct snd_ctl_elem_value ev;
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
    int ctl_fd = -1;
    char tmp[128];	

    if(!priv || !priv->ctls || priv->card < 0) {
	log_err("invalid arguments");
	return -1;
    }	
    sprintf(tmp, "/dev/snd/controlC%d", priv->card);     
    ctl_fd = open(tmp, O_RDWR);
    if(ctl_fd < 0) {
	log_err("cannot open mixer");
	return -1;
    }		   	
    for( ; nv; nv = nv->next) {
	if(!nv->name) {
	    log_info("null control name");	
	    continue;
	}
	if(!nv->value) {
	    log_info("null value for %s", nv->name);
	    continue;
	}
	for(ctl = priv->ctls; ctl; ctl = ctl->next) {
	    if(strcmp(ctl->name, nv->name) == 0) {
		memset(&ev, 0, sizeof(ev));
		ev.id.numid = ctl->numid;
		switch(ctl->type) {
		    case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
		    case SNDRV_CTL_ELEM_TYPE_INTEGER:
			for(k = 0; k < ctl->count; k++) ev.value.integer.value[k] = atoi(nv->value);
			if(ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) {
			    log_err("failed to set value for %s", nv->name);
			    break;
			}
			log_info("%s -> %s", nv->name, nv->value);
			n++;
			break;		    
		    case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
			for(i = 0; i < ctl->evcount; i++)
			    if(strcmp(ctl->evnames[i], nv->value)==0) break;
			if(i == ctl->evcount) {
			    log_err("failed to find enum value index for %s", nv->value);
			    break;
			}
			for(k = 0; k < ctl->count; k++) ev.value.enumerated.item[k] = i;
			if(ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) {
			    log_err("failed to set value for %s", nv->name);
			    break;
			}
			log_info("%s -> %s", nv->name, nv->value);
			n++;
			break;		    
		    default:
			log_err("%s: unsupported ctl type %d", nv->name, ctl->type);
			break;
	 	}
		break;
	    }
	}
    } 
    close(ctl_fd);	
    return n;    
}

static void free_mixer_controls(playback_ctx *ctx)
{
    int k;
    struct ctl_elem *ctl, *ctl_next;
    alsa_priv *priv = (alsa_priv *) ctx->alsa_priv;
	ctl = priv->ctls;
	while(ctl) {
	    ctl_next = ctl->next;
	    if(ctl->name) free(ctl->name);
	    if(ctl->evnames) {
		for(k = 0; k < ctl->evcount; k++) free(ctl->evnames[k]);
		free(ctl->evnames);	
	    }
	    free(ctl);
	    ctl = ctl_next;
	}
	priv->ctls = 0;
}


