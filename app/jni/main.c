#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <limits.h>
#ifdef ANDROID
#include <android/log.h>
#else
#define _BSD_SOURCE     1
#include <features.h>
#endif
#include <jni_sub.h>
#include "flac/decoder.h"
#include "main.h"

#if defined(ANDROID) || defined(ANDLINUX)
static char mixer_paths_file[] = "/system/etc/mixer_paths.xml";
#else
static char mixer_paths_file[PATH_MAX];
#endif

#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

static void *audio_write_thread(void *a); 

/* Called before attempting any low-level writes of audio data.
   Blocks in STATE_PAUSE/STATE_PAUSING states and never returns them.
   If the state on entry was STATE_PAUSING, attempts to pause stream
   and wakes up audio_pause() waiting for the result. 
*/

enum playback_state sync_state(playback_ctx *ctx, const char *func) {
    bool ret;
    volatile enum playback_state state;
	pthread_mutex_lock(&ctx->mutex);	
	if(ctx->state == STATE_PAUSED) {
	    log_info("%s: in paused state: blocking", func);
	    pthread_cond_wait(&ctx->cond_resumed, &ctx->mutex); /* block until PAUSED */	
	    log_info("%s: resuming after pause", func);	
	} else if(ctx->state == STATE_PAUSING) {
	    ret = alsa_is_offload(ctx) ? alsa_pause_offload(ctx) : alsa_pause(ctx);	
	    if(ret) {
		ctx->state = STATE_PAUSED;
		log_info("%s: switched to paused state: blocking", func);
	    } else log_err("failed to pause in %s", func);
	    pthread_cond_signal(&ctx->cond_paused);
	    pthread_cond_wait(&ctx->cond_resumed, &ctx->mutex);	
	    log_info("%s: resuming after pause", func);	
	}
	state = ctx->state;	
	pthread_mutex_unlock(&ctx->mutex);	
    return state;
}

void playback_complete(playback_ctx *ctx, const char *func)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->state = STATE_STOPPING;	
    log_info("playback complete in %s", func);
    pthread_mutex_unlock(&ctx->mutex);
    audio_stop(ctx);	
}

/* If this function is normally called on EOS from playback_complete() 
   in state == STATE_STOPPING, so we must wait for playback to complete.
   Otherwise it is called to request immediate stop (from java through 
   audio_init, audio_exit or audio_stop_exp) */

int audio_stop(playback_ctx *ctx) 
{
    enum playback_state in_state;

    if(!ctx) {
	log_err("no context to stop");
	return -1;
    }	
    pthread_mutex_lock(&ctx->mutex);

    in_state = ctx->state; 	
    log_info("context %p in state %d", ctx, in_state);

    if(in_state == STATE_STOPPED) {
	/* log_err("stopped already"); */
	pthread_mutex_unlock(&ctx->mutex);
	return 0;
    }		
    if(in_state == STATE_STOPPING) {
    	if(ctx->buff) buffer_stop(ctx->buff, 0);
	pthread_mutex_unlock(&ctx->mutex);
	if(ctx->audio_thread) {
	    pthread_join(ctx->audio_thread, 0);
	    ctx->audio_thread = 0;	
	}
    	alsa_stop(ctx);
	if(ctx->buff) {
	    buffer_destroy(ctx->buff);
	    ctx->buff = 0;
	}	
	ctx->state = STATE_STOPPED;
    	ctx->track_time = 0;	
	pthread_cond_broadcast(&ctx->cond_stopped);
	log_info("stopped");
	return 0;
    }
    log_info("forced stop");	
    ctx->state = alsa_is_mmapped(ctx) ? STATE_STOPPED : STATE_STOPPING;

    if(in_state == STATE_PAUSED || in_state == STATE_PAUSING) {
	log_info("context was paused brefore");
	pthread_cond_broadcast(&ctx->cond_resumed);
    }	 		
    if(ctx->buff) buffer_stop(ctx->buff, 1);
//    pthread_mutex_lock(&ctx->stop_mutex);	
//    pthread_cond_wait(&ctx->cond_stopped, &ctx->stop_mutex);
//    pthread_mutex_unlock(&ctx->stop_mutex);

    pthread_mutex_unlock(&ctx->mutex);

    return 0;
}
	

/* Stream parameters must be set by decoder before this call */
int audio_start(playback_ctx *ctx, int buffered_write)
{
    int k, ret;
    int period_size;
    const playback_format_t *format;
	
	if(!ctx) {
	    log_err("no context to start");
	    return -1;
	}
	log_info("starting context %p, state %d", ctx, ctx->state);

	pthread_mutex_lock(&ctx->mutex);
	if(ctx->state != STATE_STOPPED) {
	    log_info("context live, stopping");
	    pthread_mutex_unlock(&ctx->mutex);	
	    ret = audio_stop(ctx); 
	    pthread_mutex_lock(&ctx->mutex);
	    if(ret < 0)	return ret;
	    log_info("live context stopped");	
	}

	ret = alsa_start(ctx);
	if(ret != 0) goto err_init;

	ctx->buff = 0;
	ctx->audio_thread = 0;

	if(!buffered_write || alsa_is_mmapped(ctx)) goto done;

	/* format/period_size must be known after alsa_start() */
	format = alsa_get_format(ctx);
	if(!format) {
	    log_err("cannot determine sample format");	
	    goto err_init;
	}	
	period_size = alsa_get_period_size(ctx);

	k = (period_size > (ctx->block_max >> ctx->rate_dec)) ? 
		period_size : (ctx->block_max >> ctx->rate_dec);

/* Say, (32k * 8 * 32/8 * 64) = 64M max, reasonable (much less normally). 
Quick test for the last number for a 15-min 192/24 flac:
Nexus 5, period size 1536:
[buffer_destroy] blocked/total: writes=1385/44299 reads=36950/118131	  4
[buffer_destroy] blocked/total: writes=1167/44299 reads=30085/118131	  8
[buffer_destroy] blocked/total: writes=55/44299 reads=38319/118131	 32
[buffer_destroy] blocked/total: writes=0/44299 reads=37624/118131	 64
[buffer_destroy] blocked/total: writes=0/44299 reads=36467/118131	128
Linux pc, period size 9648:
[buffer_destroy] blocked/total: writes=0/44299 reads=18807/18807	  4
 */
	ctx->buff = buffer_create(k * ctx->channels * (format->phys_bits/8) * 64);
	if(!ctx->buff) {
	    log_err("cannot create buffer");
	    goto err_init;	
        }
	if(pthread_create(&ctx->audio_thread, 0, audio_write_thread, ctx) != 0) {
	    log_err("cannot create thread");
	    goto err_init;
	}

    done:
	ctx->state = STATE_PLAYING;
	ctx->written = 0;
	ctx->alsa_error = 0;
	pthread_mutex_unlock(&ctx->mutex);
	log_info("playback started");
	return 0;

    err_init:
	if(ctx->buff) {
	    buffer_destroy(ctx->buff);
	    ctx->buff = 0;	
	}	
	pthread_mutex_unlock(&ctx->mutex);
	return LIBLOSSLESS_ERR_INIT;	
}

/* size in frames if mmapped, in bytes otherwise */

int audio_write(playback_ctx *ctx, void *buff, int size) 
{
    enum playback_state state;
    int i;
	state = sync_state(ctx, __func__);
	if(state == STATE_STOPPED) return -1;
        i = alsa_is_mmapped(ctx) ? 
		alsa_write_mmapped(ctx, buff, size) : buffer_put(ctx->buff, buff, size);
    return (i == size) ? 0 : -1;
}

#ifdef ANDROID
static 
#endif
jboolean audio_pause(JNIEnv *env, jobject obj, jlong jctx) 
{
    bool ret;
    playback_ctx *ctx = (playback_ctx *) jctx;	
    volatile enum playback_state saved_state;
    if(!ctx) {
	log_err("no context to pause");
	return false;
    }	
    pthread_mutex_lock(&ctx->mutex);
    if(ctx->state != STATE_PLAYING) {
    	pthread_mutex_unlock(&ctx->mutex);
	log_info("not in playing state");
	return false;
    }	
    log_info("about to pause");	
    saved_state = ctx->state;	
    ctx->state = STATE_PAUSING;	
    pthread_cond_wait(&ctx->cond_paused, &ctx->mutex);	
    ret = (ctx->state == STATE_PAUSED);
    if(!ret) {
	log_err("alsa pause failed");
	ctx->state = saved_state;	
    } else log_info("paused");	
    pthread_mutex_unlock(&ctx->mutex);
    return ret;		
}

#ifdef ANDROID
static 
#endif
jboolean audio_resume(JNIEnv *env, jobject obj, jlong jctx) 
{
    bool ret;
    playback_ctx *ctx = (playback_ctx *) jctx;	
    if(!ctx) {
	log_err("no context to resume");
	return false;
    }	
    pthread_mutex_lock(&ctx->mutex);
    if(ctx->state != STATE_PAUSED) {
    	pthread_mutex_unlock(&ctx->mutex);
	log_info("not in paused state");
	return false;
    } 	
    log_info("resuming playback");	
    ret = alsa_is_offload(ctx) ? alsa_resume_offload(ctx) : alsa_resume(ctx);	
    if(!ret) log_err("resume failed, proceeding anyway");
    ctx->state = STATE_PLAYING;	
    pthread_cond_broadcast(&ctx->cond_resumed);	/* wake up writing threads or cycles */
    log_info("resumed");	
    pthread_mutex_unlock(&ctx->mutex);
    return ret;	
}


#if 0
/* to be removed */
static void thread_exit(int j) {
    log_info("signal %d received",j);	
}
#endif

#define TEST_TIMING	1

static void *audio_write_thread(void *a) 
{
    playback_ctx *ctx = (playback_ctx *) a;
    int i, k, f2b;
    const playback_format_t *format = alsa_get_format(ctx);
    void *pcm_buf = alsa_get_buffer(ctx);
    int period_size = alsa_get_period_size(ctx);
#if 0
    sigset_t set;
    struct sigaction sact = { .sa_handler = thread_exit,  };
#endif

#ifdef TEST_TIMING
    struct timeval tstart, tstop, tdiff;
    int  writes = 0, gets = 0;
    unsigned long long us_write = 0, us_get = 0;	
#endif

	if(!format) {
	    log_err("cannot determine sample format, exiting");	
	    return 0;
	}
	f2b = ctx->channels * (format->phys_bits/8);

	log_info("entering");
#if 0
	sigaction(SIGUSR1, &sact, 0);
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &set, 0);
#endif

	while(1) {
	    k = sync_state(ctx, __func__);
	    if(k != STATE_PLAYING && k != STATE_STOPPING) {
		log_err("got state %d, exiting", k);
		break;
	    }	
#ifdef TEST_TIMING
	    gettimeofday(&tstart,0);
#endif
	    pcm_buf = alsa_get_buffer(ctx);	/* may be changed after pause in sync_state! */	
	    if(!pcm_buf) {
		log_err("cannot obtain alsa buffer, exiting");	
		return 0;
	    }

	    k = buffer_get(ctx->buff, pcm_buf, period_size * f2b);

#ifdef TEST_TIMING
	    gettimeofday(&tstop,0);
	    timersub(&tstop, &tstart, &tdiff);
	    us_get += tdiff.tv_usec;
	    gets++;	
#endif
	    if(k <= 0) {
		log_info("buffer stopped or empty, exiting");
		break;
	    }

#ifdef TEST_TIMING
	    gettimeofday(&tstart,0);
#endif
	    i = alsa_write(ctx, 0, k/f2b);
#ifdef TEST_TIMING
	    gettimeofday(&tstop,0);
	    timersub(&tstop, &tstart, &tdiff);
	    us_write += tdiff.tv_usec;
	    writes++;	
#endif
	    if(i <= 0 || k != period_size * f2b) {
/*
eof detected, exiting: avg get=529 write=68784  16/44
exiting: avg get=735 write=68280 
blocked/total: writes=8/1307 reads=272/1961	
exiting: avg get=354 write=7423   24/192
blocked/total: writes=0/9481 reads=3871/28442
*/
	   	log_info("eof detected, exiting");
		break;
	    }
	}
	ctx->audio_thread = 0;
#ifdef TEST_TIMING
	if(gets && writes) log_info("exiting: avg get=%lld write=%lld", us_get/gets, us_write/writes);
#else
	log_info("exiting");
#endif
    return 0;	
}

#ifdef ANDROID
static jint audio_get_duration(JNIEnv *env, jobject obj, jlong jctx) 
{
   playback_ctx *ctx = (playback_ctx *) jctx;	
   if(!ctx || (ctx->state != STATE_PLAYING && ctx->state != STATE_PAUSED 
	&& ctx->state != STATE_PAUSING)) return 0;	
   return ctx->track_time;
}

static jboolean in_offload_mode(JNIEnv *env, jobject obj, jlong jctx)
{
   playback_ctx *ctx = (playback_ctx *) jctx;
   return alsa_is_offload(ctx) != 0;	
}

#endif

/* in seconds */
jint audio_get_cur_position(JNIEnv *env, jobject obj, jlong jctx) 
{
   playback_ctx *ctx = (playback_ctx *) jctx;
   if(!ctx) return 0;
//   if(!ctx || (ctx->state != STATE_PLAYING && ctx->state != STATE_PAUSED)) return 0;
   return alsa_is_offload(ctx) ? alsa_time_pos_offload(ctx) : ctx->written/ctx->samplerate;
}



#ifdef ANDROID
static 
#endif
jlong audio_init(JNIEnv *env, jobject obj, jlong jctx, jint card, jint device) 
{
    playback_ctx *ctx;
    playback_ctx *prev_ctx = (playback_ctx *) jctx;	
    log_info("audio_init: prev_ctx=%p, device=hw:%d,%d", prev_ctx, card, device);
    if(prev_ctx) {
	audio_stop(prev_ctx);
	if(alsa_select_device(prev_ctx, card, device) != 0) { 
	    return 0;
	}
	ctx = prev_ctx;
    } else {
	ctx = (playback_ctx *) calloc(1, sizeof(playback_ctx));
	if(!ctx) return 0;
#if !defined(ANDROID) && !defined(ANDLINUX)
	sprintf(mixer_paths_file, "%s/.alsaplayer/mixer_paths.xml", getenv("HOME"));
#endif
	ctx->xml_mixp = xml_mixp_open(mixer_paths_file);
        if(!ctx->xml_mixp) log_info("%s missing", mixer_paths_file);
        else log_info("%s opened", mixer_paths_file);

	if(alsa_select_device(ctx,card,device) != 0) {
	    free(ctx);	
	    return 0;	
	}
	pthread_mutex_init(&ctx->mutex,0);
	pthread_mutex_init(&ctx->stop_mutex,0);
	pthread_cond_init(&ctx->cond_stopped,0);
	pthread_cond_init(&ctx->cond_paused,0);
	pthread_cond_init(&ctx->cond_resumed,0);
    }
    ctx->state = STATE_STOPPED;
    ctx->track_time = 0;
    log_info("audio_init: return ctx=%p",ctx);
    return (jlong) ctx;	
}

#ifdef ANDROID
static
#endif
jboolean audio_exit(JNIEnv *env, jobject obj, jlong jctx) 
{
    playback_ctx *ctx = (playback_ctx *) jctx;	
    if(!ctx) {
	log_err("zero context");
	return false;
    }	
    log_info("audio_exit: ctx=%p",ctx);
    audio_stop(ctx);
    alsa_exit(ctx);
    alsa_free_mixer_controls(ctx);
    if(ctx->xml_mixp) xml_mixp_close(ctx->xml_mixp);
    pthread_mutex_destroy(&ctx->mutex);
    pthread_mutex_destroy(&ctx->stop_mutex);
    pthread_cond_destroy(&ctx->cond_stopped);	
    pthread_cond_destroy(&ctx->cond_paused);	
    pthread_cond_destroy(&ctx->cond_resumed);	
    free(ctx);	
    return true;
}

#ifdef ANDROID
static 
#endif
jboolean audio_decrease_volume(JNIEnv *env, jobject obj, jlong jctx) 
{
    playback_ctx *ctx = (playback_ctx *) jctx;	
    if(!ctx) {
	log_err("no context");
	return false;
    }
    pthread_mutex_lock(&ctx->mutex);
    alsa_decrease_volume(ctx);	
    pthread_mutex_unlock(&ctx->mutex);
    return true;
}

#ifdef ANDROID
static 
#endif
jboolean audio_increase_volume(JNIEnv *env, jobject obj, jlong jctx) 
{
    playback_ctx *ctx = (playback_ctx *) jctx;	
    if(!ctx) {
	log_err("no context");
	return false;
    }
    pthread_mutex_lock(&ctx->mutex);
    alsa_increase_volume(ctx);	
    pthread_mutex_unlock(&ctx->mutex);
    return true;
}

extern jint audio_play(JNIEnv *env, jobject obj, playback_ctx *ctx, jstring jfile, jint format, jint start) 
{
    int ret = 0;
	if(!ctx) {
	    log_err("no context");      
	    return LIBLOSSLESS_ERR_NOCTX;
	}
	ctx->file_format = format;
	switch(format) {
	    case FORMAT_FLAC:
		ret = flac_play(env, obj, ctx, jfile, start);
		break;
	    case FORMAT_APE:
		ret = ape_play(env, obj, ctx, jfile, start);
		break;
	    case FORMAT_MP3:
		ret = mp3_play(env, obj, ctx, jfile, start);
		break;
	    case FORMAT_WAV:
		ret = wav_play(env, obj, ctx, jfile, start); 	
		break;
	    case FORMAT_ALAC:
		ret = alac_play(env, obj, ctx, jfile, start); 	
		break;
	    default:
		ret = LIBLOSSLESS_ERR_FORMAT;
		break;
	}
	if(ret) log_err("exiting on error %d", ret);
	else log_info("Playback complete.");

    return ret;
}

////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////

#ifdef ANDROID

#ifndef CLASS_NAME
#error "CLASS_NAME not set in Android.mk"
#endif

/* For simplicity, a java string array is returned rather than array of structures. 
   See alsa_get_devices() for details. */

static jobjectArray get_devices(JNIEnv *env, jobject obj)
{
    int i, n = 0;
    jobjectArray ja = 0;
    char **devs;
	n = alsa_get_devices(&devs);
	if(!n) return 0;
	ja = (*env)->NewObjectArray(env, n, (*env)->FindClass(env, "java/lang/String"), 0);
	for(i = 0; i < n; i++) {
	    (*env)->SetObjectArrayElement(env, ja, i, (*env)->NewStringUTF(env, devs[i]));
	    free(devs[i]);
	}
	free(devs);
    return ja;
}

static jstring current_device_info(JNIEnv *env, jobject obj, jlong jctx)
{
    playback_ctx *ctx = (playback_ctx *) jctx;	
    char *c = alsa_current_device_info(ctx);
    return c ? (*env)->NewStringUTF(env,c) : 0;
}

static jboolean libinit(JNIEnv *env, jobject obj, jint sdk) 
{
    return true;
}

static jboolean libexit(JNIEnv *env, jobject obj) 
{
    return true;
}

static JavaVM *gvm;
static jobject giface; 

void update_track_time(JNIEnv *env, jobject obj, int time) 
{
     jclass cls;
     jmethodID mid;
     bool attached = false;
     JNIEnv *envy;

	if((*gvm)->GetEnv(gvm, (void **)&envy, JNI_VERSION_1_4) != JNI_OK) {
            log_err("GetEnv FAILED");
	     if((*gvm)->AttachCurrentThread(gvm, &envy, NULL) != JNI_OK) {
            	log_err("AttachCurrentThread FAILED");
		     return;
	     }	
	     attached = true;	
	}
	cls = (*envy)->GetObjectClass(envy,giface);
	if(!cls) {
          log_err("failed to get class iface");
	  return;  	
	}
        mid = (*env)->GetStaticMethodID(envy, cls, "updateTrackLen", "(I)V");
        if(mid == NULL) {
	  log_err("Cannot find java callback to update time");
         return; 
        }
	(*envy)->CallStaticVoidMethod(envy,cls,mid,time);
	if(attached) (*gvm)->DetachCurrentThread(gvm);
}

static jboolean audio_stop_exp(JNIEnv *env, jobject obj, jlong jctx) 
{
    playback_ctx *ctx = (playback_ctx *) jctx;	
    return (audio_stop(ctx) == 0);    	
}

static jint audio_play_exp(JNIEnv *env, jobject obj, jlong jctx, jstring jfile, jint format, jint start) 
{
    playback_ctx *ctx = (playback_ctx *) jctx;	
    return audio_play(env, obj, ctx, jfile, format, start);	
}

static JNINativeMethod methods[] = {
 { "audioInit", "(JII)J", (void *) audio_init },
 { "audioExit", "(J)Z", (void *) audio_exit },
 { "audioStop", "(J)Z", (void *) audio_stop_exp },
 { "audioPause", "(J)Z", (void *) audio_pause },
 { "audioResume", "(J)Z", (void *) audio_resume },
 { "audioGetDuration", "(J)I", (void *) audio_get_duration },
 { "audioGetCurPosition", "(J)I", (void *) audio_get_cur_position },
 { "audioDecreaseVolume", "(J)Z", (void *) audio_decrease_volume },
 { "audioIncreaseVolume", "(J)Z", (void *) audio_increase_volume },
 { "audioPlay", "(JLjava/lang/String;II)I", (void *) audio_play_exp },
 { "extractFlacCUE", "(Ljava/lang/String;)[I", (void *) extract_flac_cue },
 { "getAlsaDevices", "()[Ljava/lang/String;", (void *) get_devices },
 { "getCurrentDeviceInfo", "(J)Ljava/lang/String;", (void *) current_device_info },
 { "isUsbCard", "(I)Z", (void *) alsa_is_usb_card },
 { "inOffloadMode", "(J)Z", (void *) in_offload_mode },
 { "isOffloadDevice", "(II)Z", (void *) alsa_is_offload_device },
 { "libInit", "(I)Z", (void *) libinit },
 { "libExit", "()Z", (void *) libexit },
};

jint JNI_OnLoad(JavaVM* vm, void* reserved) 
{
    jclass clazz = NULL;
    JNIEnv* env = NULL;
    jmethodID constr = NULL;
    jobject obj = NULL;

      log_info("JNI_OnLoad");
      gvm = vm;	

      if((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_4) != JNI_OK) {
        log_err("GetEnv FAILED");
        return -1;
      }
      prctl(PR_SET_DUMPABLE, 1);
      clazz = (*env)->FindClass(env, CLASS_NAME);
      if(!clazz) {
        log_err("Registration unable to find class '%s'", CLASS_NAME);
        return -1;
      }
      constr = (*env)->GetMethodID(env, clazz, "<init>", "()V");
      if(!constr) {
        log_err("Failed to get constructor");
	return -1;
      }
      obj = (*env)->NewObject(env, clazz, constr);
      if(!obj) {
        log_err("Failed to create an interface object");
	return -1;
      }
      giface = (*env)->NewGlobalRef(env,obj);

      if((*env)->RegisterNatives(env, clazz, methods, sizeof(methods) / sizeof(methods[0])) < 0) {
        log_err("Registration failed for '%s'", CLASS_NAME);
        return -1;
      }
    
   return JNI_VERSION_1_4;
}
#else
void update_track_time(JNIEnv *env, jobject obj, int time) { }
#endif


