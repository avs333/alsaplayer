#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#ifdef ANDROID
#include <android/log.h>
#endif
#include <jni_sub.h>
#include "main.h"

#define BUFF_FILL_TEST	1

/* TODO: elmimnate memcpys on buffer_get() */
struct pcm_buffer_t {
    void *mem;
    int size;
    int start_idx;              /* index of first valid byte to read */
    int bytes;                  /* bytes currently occupying buffer */
    volatile int should_run;
    volatile int abort;         /* terminate immediately */
    pthread_mutex_t mutex;
    pthread_mutex_t mutex_cond;
    pthread_cond_t  buff_changed;
    pthread_t reader;   
#ifdef BUFF_FILL_TEST
    int total_puts, blocked_puts;
    int total_gets, blocked_gets;
#endif
};

pcm_buffer *buffer_create(int size) 
{
    pcm_buffer *buff = (pcm_buffer *) calloc(1, sizeof(pcm_buffer));
    if(!buff) return 0;	
    buff->size = size;	
    buff->mem = malloc(size);
    if(!buff->mem) {
	free(buff);
	return 0;
    }
    pthread_mutex_init(&buff->mutex, 0);	
    pthread_mutex_init(&buff->mutex_cond, 0);	
    pthread_cond_init(&buff->buff_changed, 0);
    buff->should_run = 1;
    return buff;
}

void buffer_destroy(pcm_buffer *buff)
{
    if(buff) {
#ifdef BUFF_FILL_TEST
	log_info("blocked/total: writes=%d/%d reads=%d/%d", buff->blocked_puts, buff->total_puts, 
	    buff->blocked_gets, buff->total_gets);
#endif
	if(buff->mem) free(buff->mem);
	pthread_mutex_destroy(&buff->mutex);
	pthread_mutex_destroy(&buff->mutex_cond);
	pthread_cond_destroy(&buff->buff_changed);
	free(buff);
    }
}

static inline void signal_buffer_changed(pcm_buffer *buff) 
{
    pthread_mutex_lock(&buff->mutex_cond);
    pthread_cond_broadcast(&buff->buff_changed);
    pthread_mutex_unlock(&buff->mutex_cond);
}

/* To be called with mutex locked */
void buffer_stop(pcm_buffer *buff, int now)
{
    pthread_mutex_lock(&buff->mutex);
    if(!buff->should_run) {
	pthread_mutex_unlock(&buff->mutex);
	return;
    }		
    log_info("stopping, now=%d", now);	
    buff->should_run = 0;
    if(now) buff->abort = 1;
    pthread_mutex_unlock(&buff->mutex);
    signal_buffer_changed(buff);
    usleep(15);	
    signal_buffer_changed(buff);
    log_info("stopped run=%d abort=%d", buff->should_run, buff->abort);	
}

/* Returns negative on error, zero on stop or abort, 
   otherwise the same nubmer of bytes as requested */

int buffer_put(pcm_buffer *buff, void *src, int bytes) 
{
    int end_idx, k;

    if(!buff || bytes <= 0 || bytes > buff->size 
	|| !buff->should_run || buff->abort) return -1;

    pthread_mutex_lock(&buff->mutex);
#ifdef BUFF_FILL_TEST
    buff->total_puts++;	
    if(buff->size - buff->bytes <  bytes) buff->blocked_puts++;	
#endif
    while(buff->size - buff->bytes <  bytes && buff->should_run && !buff->abort) {
	pthread_mutex_unlock(&buff->mutex);
	pthread_mutex_lock(&buff->mutex_cond);
	pthread_cond_signal(&buff->buff_changed);
	pthread_cond_wait(&buff->buff_changed, &buff->mutex_cond);
	pthread_mutex_unlock(&buff->mutex_cond);
	pthread_mutex_lock(&buff->mutex);
    }	
    if(!buff->should_run || buff->abort) {
    	pthread_mutex_unlock(&buff->mutex);	  
	return 0;	
    }
    end_idx = buff->start_idx + buff->bytes;
    if(end_idx >= buff->size) end_idx -= buff->size;
    if(end_idx + bytes > buff->size) {
	k = buff->size - end_idx;
	memcpy(buff->mem + end_idx, src, k);
	memcpy(buff->mem, src + k, bytes - k);
    } else memcpy(buff->mem + end_idx, src, bytes);
    buff->bytes += bytes;	    	

    pthread_mutex_unlock(&buff->mutex);	  
    signal_buffer_changed(buff);	
     		
    return bytes;  
}

/* Returns negative on error, zero on abort, 
   or number of bytes that may be less than requested 
   if should_run = 0 */

int buffer_get(pcm_buffer *buff, void *dst, int bytes)
{
    int k;     	

    if(!buff || bytes <= 0 || bytes > buff->size || buff->abort) return -1;
    
    pthread_mutex_lock(&buff->mutex);
#ifdef BUFF_FILL_TEST
    buff->total_gets++;
    if(bytes > buff->bytes) buff->blocked_gets++;	
#endif
    while(bytes > buff->bytes && buff->should_run && !buff->abort) {
	pthread_mutex_unlock(&buff->mutex);
	pthread_mutex_lock(&buff->mutex_cond);
	pthread_cond_signal(&buff->buff_changed);
	pthread_cond_wait(&buff->buff_changed, &buff->mutex_cond);
	pthread_mutex_unlock(&buff->mutex_cond);
	pthread_mutex_lock(&buff->mutex);
    }	
    if(buff->abort) {
	log_info("aborted");
    	pthread_mutex_unlock(&buff->mutex);	  
	return 0;	
    }
    if(bytes > buff->bytes) bytes = buff->bytes; /* end of stream */
    if(!buff->should_run) log_info("last bytes: %d", buff->bytes);	
    k = buff->size - buff->start_idx;
    if(k < bytes) {
	memcpy(dst, buff->mem + buff->start_idx, k);
	memcpy(dst + k, buff->mem, bytes - k);
	buff->start_idx = bytes - k;
    } else {
	memcpy(dst, buff->mem + buff->start_idx, bytes);
	buff->start_idx += bytes;
	if(buff->start_idx == buff->size) buff->start_idx = 0;
    }	
    buff->bytes -= bytes;	
    pthread_mutex_unlock(&buff->mutex);	  
    signal_buffer_changed(buff); 	

    return bytes;
}


