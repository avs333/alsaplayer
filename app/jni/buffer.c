#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/time.h>
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

pcm_buffer *pcm_buffer_create(int size) 
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

void pcm_buffer_destroy(pcm_buffer *buff)
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
void pcm_buffer_stop(pcm_buffer *buff, int now)
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

int pcm_buffer_put(pcm_buffer *buff, void *src, int bytes) 
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

int pcm_buffer_get(pcm_buffer *buff, void *dst, int bytes)
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
#if 0
    if(!buff->should_run) log_info("last bytes: %d", buff->bytes);	
#endif
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


/*************** Below assumes a single reader and a single writer! **********/
	
struct blk_buffer_t {
    void **base;
    int count;
    int head, tail, elts;	/* elts = number of valid elements in buffer */
    volatile int should_run;
    volatile int abort;         /* terminate immediately */
    pthread_mutex_t mutex;
    pthread_mutex_t mutex_cond;
    pthread_cond_t  buff_changed;
    pthread_t output_thread;
#ifdef BUFF_FILL_TEST
    int total_puts, blocked_puts;
    int total_gets, blocked_gets;
#endif
};

blk_buffer *blk_buffer_create(int buffsz, int count) 
{
    int i, k;
    blk_buffer *buff;

    buff = (blk_buffer *) calloc(1, sizeof(blk_buffer));
    if(!buff) {
	log_err("no memory");
	return 0;
    }		
    buff->count = count;	
    buff->base = malloc(count * sizeof(void *));
    if(!buff->base) {
	free(buff); 
	log_err("no memory");
	return 0;	
    }
    for(i = 0; i < count; i++) {
	buff->base[i] = malloc(buffsz);
	if(!buff->base[i]) {
	    for(k = 0; k < i; k++) free(buff->base[k]);
	    free(buff);	
	    log_err("no memory for buffers");
	    return 0;	
	}
    }	
    pthread_mutex_init(&buff->mutex, 0);	
    pthread_mutex_init(&buff->mutex_cond, 0);	
    pthread_cond_init(&buff->buff_changed, 0);
    buff->should_run = 1;

    return buff;
}

void blk_buffer_destroy(blk_buffer *buff)
{
    int i;
    if(buff) {
#ifdef BUFF_FILL_TEST
	log_info("blocked/total: writes=%d/%d reads=%d/%d", buff->blocked_puts, buff->total_puts, 
	    buff->blocked_gets, buff->total_gets);
#endif
	for(i = 0; i < buff->count; i++) 
		if(buff->base[i]) free(buff->base[i]);
	if(buff->base) free(buff->base);
	pthread_mutex_destroy(&buff->mutex);
	pthread_mutex_destroy(&buff->mutex_cond);
	pthread_cond_destroy(&buff->buff_changed);
	free(buff);
    }
}

static inline void signal_blk_buffer_changed(blk_buffer *buff) 
{
    pthread_mutex_lock(&buff->mutex_cond);
    pthread_cond_broadcast(&buff->buff_changed);
    pthread_mutex_unlock(&buff->mutex_cond);
}

/* To be called with mutex locked */
void blk_buffer_stop(blk_buffer *buff, int now)
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
    signal_blk_buffer_changed(buff);
    usleep(15);	
    signal_blk_buffer_changed(buff);
    log_info("stopped run=%d abort=%d", buff->should_run, buff->abort);	
}

void *blk_buffer_request_decoding(blk_buffer *buff) 
{
    int bp = 0;
    if(!buff) return 0;	
#ifdef BUFF_FILL_TEST
    buff->total_puts++;	
#endif
    pthread_mutex_lock(&buff->mutex);
    while(buff->elts == buff->count && buff->should_run && !buff->abort) {
#ifdef BUFF_FILL_TEST
	if(!bp) { 
	     buff->blocked_puts++;
	     bp++;
	} 		
#endif
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
    pthread_mutex_unlock(&buff->mutex);	  
    return buff->base[buff->head];
}

void blk_buffer_commit_decoding(blk_buffer *buff)
{
    pthread_mutex_lock(&buff->mutex);
    buff->head++;
    if(buff->head == buff->count) buff->head = 0;	
    buff->elts++;
    pthread_mutex_unlock(&buff->mutex);	  
    signal_blk_buffer_changed(buff);
}

void *blk_buffer_request_playback(blk_buffer *buff)
{
    int bp = 0;
    if(!buff) return 0;	
#ifdef BUFF_FILL_TEST
    buff->total_gets++;	
#endif
    pthread_mutex_lock(&buff->mutex);
    while(!buff->elts && buff->should_run && !buff->abort) {
#ifdef BUFF_FILL_TEST
	if(!bp) { 
	     buff->blocked_gets++;
	     bp++;
	} 		
#endif
	pthread_mutex_unlock(&buff->mutex);
	pthread_mutex_lock(&buff->mutex_cond);
	pthread_cond_signal(&buff->buff_changed); 
	pthread_cond_wait(&buff->buff_changed, &buff->mutex_cond);
	pthread_mutex_unlock(&buff->mutex_cond);
	pthread_mutex_lock(&buff->mutex);
    }
    if(buff->abort) {
	pthread_mutex_unlock(&buff->mutex);	  
	return 0;	
    }
    if(!buff->should_run) {
	if(!buff->elts) {
	    log_info("no more buffers, exiting");
	    pthread_mutex_unlock(&buff->mutex);	  
	    return 0;
	} else log_info("last buffers %d", buff->elts);	
    }
    pthread_mutex_unlock(&buff->mutex);	  
    return buff->base[buff->tail];
}

void blk_buffer_commit_playback(blk_buffer *buff)
{
    pthread_mutex_lock(&buff->mutex);
    buff->tail++;
    if(buff->tail == buff->count) buff->tail = 0;	
    buff->elts--;
    pthread_mutex_unlock(&buff->mutex);	  
    signal_blk_buffer_changed(buff);
}


