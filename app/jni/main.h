#ifndef _MAIN_H_INCLUDED
#define _MAIN_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ANDROID
#ifndef log_info
#if 1
#define log_info(fmt, args...)  __android_log_print(ANDROID_LOG_INFO, "liblossless", "%d [%s] " fmt, gettid(), __func__, ##args)
#else
#define log_info(...)
#endif
#define log_err(fmt, args...)   __android_log_print(ANDROID_LOG_ERROR, "liblossless", "%d [%s] " fmt, gettid(),  __func__, ##args)
#endif
#else
extern int quiet_run;
#define log_info(fmt, args...) do { if(!quiet_run) printf("liblossless: [%s] " fmt "\n", __func__, ##args); } while(0)
#define log_err(fmt, args...)  do { fprintf(stderr, "liblossless: [%s] " fmt "\n", __func__, ##args); } while(0)
#endif

#ifndef timeradd
# define timeradd(a, b, result)                                               \
  do {                                                                        \
    (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;                             \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;                          \
    if ((result)->tv_usec >= 1000000)                                         \
      {                                                                       \
        ++(result)->tv_sec;                                                   \
        (result)->tv_usec -= 1000000;                                         \
      }                                                                       \
  } while (0)
#endif


typedef int snd_pcm_format_t;

typedef struct pcm_buffer_t pcm_buffer;	/* private struct pcm_buffer_t is defined in buffer.c */

typedef struct _playback_format_t {
    snd_pcm_format_t fmt;
    int mask;
    int phys_bits;
    int strm_bits;
    const char *str;
} playback_format_t;

struct pcm_buffer_t;

#define FORMAT_WAV	0
#define FORMAT_FLAC	1
#define FORMAT_APE	2
#define FORMAT_MP3	3	/* offload playback only */

typedef struct {
   enum _playback_state_t {
	STATE_STOPPED = 0,	/* init state */
	STATE_PLAYING,
	STATE_PAUSED
   } state;
   int  track_time;			/* set by decoder */
   int  file_format;			/* FORMAT_* above, set on entry to audio_play */
   int  channels, bps;			/* set by decoder */
   int  samplerate;			/* playback samplerate. set by decoder initially, but may be scaled down by 2^n by */
   int  rate_dec;			/* alsa if it's not supported by hw, i.e.: samplerate = (file_samplerate >> rate_dec) */
   int  block_min, block_max;		/* set by decoder */
   int  frame_min, frame_max;		/* set by decoder */
   int  bitrate;			/* set by decoder */	
   int  written;			/* set by audio thread */	
   void *xml_mixp;			/* descriptor for xml file with device controls ("/system/etc/mixer_paths.xml" or similar) */
   void *ctls;				/* cached mixer controls for current card */
   pthread_mutex_t mutex, stop_mutex;
   pthread_t audio_thread;
   pthread_cond_t cond_stopped;		/* audio_play() is about to exit */
   int  stopped; 			/* associated variable */
   pthread_cond_t cond_resumed;		/* thread will pause waiting for state change */
   struct pcm_buffer_t *buff; 
   void *alsa_priv;
   int  alsa_error;			/* set on error exit from alsa thread  */
} playback_ctx;

/* main.c */
extern int audio_start(playback_ctx *ctx);
extern int audio_stop(playback_ctx *ctx, int abort);
extern int audio_write(playback_ctx *ctx, void *buff, int size);
extern int check_state(playback_ctx *ctx, const char *func);
extern void update_track_time(JNIEnv *env, jobject obj, int time);
extern int sync_state(playback_ctx *ctx, const char *func);
extern jint audio_play(JNIEnv *env, jobject obj, playback_ctx* ctx, jstring jfile, jint format, jint start);
#ifndef ANDROID
extern jint audio_init(JNIEnv *env, jobject obj, playback_ctx *prev_ctx, jint card, jint device);
extern jboolean audio_exit(JNIEnv *env, jobject obj, playback_ctx *ctx);
extern jboolean audio_pause(JNIEnv *env, jobject obj, playback_ctx *ctx);
extern jboolean audio_resume(JNIEnv *env, jobject obj, playback_ctx *ctx);
#endif

/* alsa.c */
extern int alsa_select_device(playback_ctx *ctx, int card, int device);
extern int alsa_start(playback_ctx *ctx);
extern void alsa_stop(playback_ctx *ctx);
extern ssize_t alsa_write(playback_ctx *ctx, size_t count);
extern bool alsa_pause(playback_ctx *ctx);
extern bool alsa_resume(playback_ctx *ctx);
extern bool alsa_set_default_volume(playback_ctx *ctx);
extern bool alsa_increase_volume(playback_ctx *ctx);
extern bool alsa_decrease_volume(playback_ctx *ctx);
extern void alsa_exit(playback_ctx *ctx);
extern void alsa_free_mixer_controls(playback_ctx *ctx);
extern void *alsa_get_buffer(playback_ctx *ctx);
extern int alsa_get_period_size(playback_ctx *ctx);
extern const playback_format_t *alsa_get_format(playback_ctx *ctx);
extern int alsa_is_offload(playback_ctx *ctx);
#ifdef ANDROID
extern int alsa_get_devices(char ***dev_names);
extern int alsa_is_usb_card(JNIEnv *env, jobject obj, int card);
extern int alsa_is_offload_device(JNIEnv *env, jobject obj, int card, int device);
#endif

/* alsa_offload.c */
extern int alsa_play_offload(playback_ctx *ctx, int fd, off_t start_offset);
extern bool alsa_pause_offload(playback_ctx *ctx);
extern bool alsa_resume_offload(playback_ctx *ctx);
extern int alsa_time_pos_offload(playback_ctx *ctx);
extern int mp3_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start);

/* buffer.c */
extern pcm_buffer *buffer_create(int size);
extern int buffer_put(pcm_buffer *buff, void *src, int bytes);
extern int buffer_get(pcm_buffer *buff, void *dst, int bytes);
extern void buffer_stop(pcm_buffer *buff, int now);	/* Stop accepting new frames. If now == 1, stop providing new frames as well. */
extern void buffer_destroy(pcm_buffer *buff);

/* flac/main.c */
extern int flac_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start);
extern JNIEXPORT jintArray JNICALL extract_flac_cue(JNIEnv *env, jobject obj, jstring jfile);

/* ape/main.c */
extern int ape_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, int start);

/* tinyxml2/xmlparser.cpp */
struct nvset {
    const char *name;
    const char *value;
    int min, max;
    struct nvset *next;
};

static inline void free_nvset(struct nvset *nv) {
    while(nv) {	
	struct nvset *next = nv->next;
	    free(nv);
	    nv = next;
    }	
}
/* for mixer_paths.xml */
extern void *xml_mixp_open(const char *xml_path);
extern void xml_mixp_close(void *xml);
extern const char *xml_mixp_find_control_default(void *xml, const char *name);
extern struct nvset *xml_mixp_find_control_set(void *xml, const char *path);
/* for cards.xml */
extern void *xml_dev_open(const char *xml_path, const char *card, int device);
extern void xml_dev_close(void *xml);
extern int xml_dev_is_builtin(void *xml);
extern int xml_dev_is_offload(void *xml);
extern int xml_dev_is_mmapped(void *xml);
extern int xml_dev_exists(void *xml, int device);  /* used with device=-1 in xml_dev_open */	
extern struct nvset *xml_dev_find_ctls(void *xml, const char *name, const char *value);

/* TODO
extern int xml_dev_get_chunks(void *xml, const char *rate, const char *format);
extern int xml_dev_get_chunk_size(void *xml, const char *rate, const char *format);
*/

#define LIBLOSSLESS_ERR_NOCTX		1
#define LIBLOSSLESS_ERR_INV_PARM	2
#define LIBLOSSLESS_ERR_NOFILE		3
#define LIBLOSSLESS_ERR_FORMAT		4
#define LIBLOSSLESS_ERR_AU_GETCONF 	5	
#define LIBLOSSLESS_ERR_AU_SETCONF	6
#define LIBLOSSLESS_ERR_AU_BUFF		7
#define LIBLOSSLESS_ERR_AU_SETUP	8
#define LIBLOSSLESS_ERR_AU_START	9
#define LIBLOSSLESS_ERR_IO_WRITE 	10	
#define LIBLOSSLESS_ERR_IO_READ		11
#define LIBLOSSLESS_ERR_DECODE		12 
#define LIBLOSSLESS_ERR_OFFSET		13
#define LIBLOSSLESS_ERR_NOMEM		14
#define LIBLOSSLESS_ERR_INIT		15

#ifdef __cplusplus
}
#endif

#endif
