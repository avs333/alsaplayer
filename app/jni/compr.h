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
#include <sys/time.h>
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
#include "alsa_priv.h"

#ifdef _COMPR_PROTO_
#undef _FN
#define ___FN(NAME,A) NAME ## _ ## A
#define __FN(NAME,A) ___FN(NAME,A)
#define _FN(NAME) __FN(NAME,_COMPR_PROTO_)
#else
#undef _FN
#define _FN(NAME) (*NAME)
#endif


extern int compr_get_version(playback_ctx *ctx, int fd);
extern int _FN(compr_fmt_check) (int fmt, uint64_t codecs_mask); 
extern int _FN(compr_get_codecs) (int fd, int **codecs); 
extern int _FN(compr_get_caps) (int fd, int *min_fragments, int *max_fragments, int *min_fragment_size, int *max_fragment_size); 
extern int _FN(compr_set_hw_params) (playback_ctx *ctx, int fd, int chunks, int chunk_size, int force);
extern int _FN(compr_start_playback) (int fd);
extern int _FN(compr_drain) (int fd);
extern int _FN(compr_pause) (int fd);
extern int _FN(compr_resume) (int fd);
extern int _FN(compr_avail) (int fd, int *avail);
extern int _FN(compr_offload_time_pos) (int fd);



