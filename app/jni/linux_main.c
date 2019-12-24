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
#include <pthread.h>
#include <dlfcn.h>
#include <jni_sub.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>
#include "flac/decoder.h"
#include "main.h"

#define _BSD_SOURCE	1
#include <features.h>
#include <sys/time.h>
#define __USE_BSD  1
#include <getopt.h>

#ifdef ANDLINUX
extern int security_getenforce(void);
extern int security_setenforce(int value);
#endif

int quiet_run = 0;
static int need_show_time = 0;
static jlong ctx = 0;

struct call_args {
    int min, sec, ftype; 	
    char *file;	
    int track;
};

void bye(int sig) 
{
    ((playback_ctx *)ctx)->state = STATE_INTR;
}

static int usage(char *prog) 
{
   printf("Usage: %s [-x file] [-c card] [-d device] (-i | [-s min:sec | -t track_no] [-p num:sz] [-q] file)\n", prog);
   return printf(
#ifdef ANDLINUX
		 "-x\tspecify custom xml config (default is /sdcard/.alsaplayer/cards.xml)\n"
#else
		 "-x\tspecify custom xml config (default is $HOME/.alsaplayer/cards.xml)\n"
#endif
		 "-c/-d\tcard/device numbers, see output of 'cat /proc/asound/pcm' (defaults are 0/0)\n"
		 "-s\tspecify start point of playback in a file\n"
		 "-t\tspecify cue file track (audio file defined in cue must be in the same dir)\n"
		 "-p\tforce number and size of periods (in frames) or fragments (in bytes)\n"
		 "-m\tforce memory-mapped playback\n"
		 "-r\tforce using ring buffer instead of block buffer\n"
		 "-q\tquiet mode, suppress extra info\n"
		 "-i\ttest the selected device and show its information\n"
		 "-w\tshow stream time\n"
	);
}

void *thd(void *a) 
{
    struct call_args *args = (struct call_args *) a;
    sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	audio_play(0, 0, (playback_ctx *) ctx, args->file, args->ftype, args->min*60 + args->sec);	
    return 0;	
}

void *time_thd(void *a)
{
    int sec;
    while(need_show_time) {
	if(!ctx) break;
	sec = audio_get_cur_position(0, 0, ctx);
	printf("sec = %d\n", sec);
	sleep(1);
    }
    return 0; 
}

void pause_resume(int sig) {
    if(sig == SIGUSR1) audio_pause(0,0,ctx);
    else audio_resume(0,0,ctx);
}

static int parse_cue(struct call_args *args);

static int test_device(int card, int device)
{
    ctx = audio_init(0, 0, 0, card, device);
    if(!ctx) return 1;
    printf("%s\n", alsa_current_device_info((playback_ctx *)ctx));	    	
    audio_exit(0, 0, ctx);
    return 0; 		
}


int main(int argc, char **argv)
{
    int card = 0, device = 0, info = 0, opt;
    char *c;
    pthread_t thread, time_thread;
    sigset_t set;
    struct call_args args[1];

	memset(args, 0, sizeof(struct call_args));
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);

	signal(SIGINT, bye);
	signal(SIGUSR1, pause_resume);	
	signal(SIGUSR2, pause_resume);	


	while ((opt = getopt(argc, argv, "c:d:s:t:qix:p:wmr")) != -1) {
	    switch (opt) {
		case 'c':
		    card = atoi(optarg);
		    break;
		case 'd':
		    device = atoi(optarg);
		    break;
		case 's':
		    c = strchr(optarg, ':');
		    if(!c) return printf("invalid time spec\n");
		    args->min = atoi(optarg);
		    args->sec = atoi(c+1);
		    break;
		case 'p':
		    c = strchr(optarg, ':');
		    if(!c) return printf("bad argument to -p option\n");
		    forced_chunks = atoi(optarg);
		    forced_chunk_size = atoi(c+1);
		    if(!forced_chunks || !forced_chunk_size)
			return printf("invalid number/size of periods/fragments %d/%d\n", 
				forced_chunks, forced_chunk_size);	
		    break;
		case 't':
		    args->track = atoi(optarg);
		    break; 
		case 'm':
		    force_mmap = 1;
		    break;	
		case 'w':
		    need_show_time = 1;
		    break;	
		case 'q':
		    quiet_run = 1;
		    break;
		case 'i':
		    info = 1;
		    break;
		case 'r':
		    force_ring_buffer = 1;
		    break;
		case 'x':
		    ext_cards_file = optarg;
		    break;			
		default: /* '?' */
		    return usage(argv[0]);
	    }
	}
#ifdef ANDLINUX
	if(security_getenforce() == 1) {
	    if(!quiet_run) printf("switching to permissive mode: ");	
	    security_setenforce(0);
	    if(!quiet_run) printf(security_getenforce() == 0 ? "okay\n" : "failed\n");	
	}
#endif
	if(info) {
	    quiet_run = 1; 
	    return test_device(card, device);
	}
	if(optind >= argc) {
	   printf("No file specified\n");
	   return usage(argv[0]);
	}
again:
	args->file = strdup(argv[optind]);
	args->ftype = -1;
	c = strrchr(args->file, '.');
	if(c) {
	    if(strcmp(c, ".flac") == 0) {
		args->ftype = FORMAT_FLAC;
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".ape") == 0) {
		args->ftype = FORMAT_APE;	
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".m4a") == 0) {
		args->ftype = FORMAT_ALAC;	
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".wav") == 0) {
		args->ftype = FORMAT_WAV;	
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".mp3") == 0) {
		args->ftype = FORMAT_MP3;	
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".cue") == 0) {
		if(args->min || args->sec) printf("start time specificaton ignored for cue file\n");
		if(parse_cue(args) != 0) {
		    if(args->file) free(args->file);	
		    return -1;
		}
	    }
	}

	if(args->ftype == -1) return printf("file extension must be .flac, .ape or .mp3\n");
	ctx = audio_init(0, 0, 0, card, device);
	if(!ctx) return -1;	

	pthread_create(&thread, 0, thd, args);
	if(need_show_time) pthread_create(&time_thread, 0, time_thd, 0);

	pthread_join(thread, 0);
	if(need_show_time) {
	    need_show_time = 0;
	    pthread_join(time_thread, 0);	
	}
	audio_exit(0, 0, ctx);
	if(args->file) free(args->file);

	optind++;
        if(optind < argc) goto again;

    return 0;
}

static int parse_cue(struct call_args *args) {

    char *c, *ce, buff[PATH_MAX], track[64], *title = 0;	
    int found_track = 0;
    struct stat st;
    FILE *f = fopen(args->file, "r");	

	if(!f) return printf("cannot open %s\n", args->file);

	sprintf(track, "TRACK %02d AUDIO", args->track);

	while(fgets(buff, sizeof(buff), f)) {
	    if(strncmp(buff, "FILE ", 5) == 0) {
		c = strrchr(buff, ' ');
		if(!c) continue;
		if(strncmp(c+1, "WAVE", 4) != 0 && 
		    strncmp(c+1, "FLAC", 4) != 0 && strncmp(c+1, "APE", 3) != 0 /* yeah.. some guys create such cues */
		) continue;
		if(buff[5] == '"' && *(c - 1) == '"') {
		    *(c-1) = 0;
		    c = buff+6;	
		} else {
		    *c = 0;
		    c = buff+5;	
		}
		ce = strrchr(args->file, '/');
		if(ce) {	/* prepend pathname */
		    *++ce = 0; 
		    args->file = (char *) realloc(args->file, strlen(args->file) + strlen(c) + 4);
		    strcat(args->file, c);
		} else {
		    free(args->file);
		    args->file = strdup(c);
		    args->file = (char *) realloc(args->file, strlen(args->file) + 4); /* may need to change extension */	
		}
		ce = strrchr(args->file, '.');
		if(!ce) return printf("error: cue references file %s with no extension\n", args->file);
		if(stat(args->file, &st) != 0) { 
		    const struct {
			const char *ext; 
			int ftype;
		    } *ep, ee[] = { { ".flac", FORMAT_FLAC }, { ".ape", FORMAT_APE }, { ".mp3", FORMAT_MP3 }, { ".wav", FORMAT_WAV }, { 0, 0 }  };
		    for(ep = &ee[0]; ep->ext; ep++) {
			*ce = 0; 
			strcat(args->file, ep->ext);
		    	if(stat(args->file, &st) == 0) {
			    args->ftype = ep->ftype;
			    break;
			}
		    }
		    if(!ep->ext) return printf("error: cue references non-existing file %s\n", args->file);		
		} else {
		    if(strcmp(ce, ".flac") == 0) args->ftype = FORMAT_FLAC;
		    else if(strcmp(ce, ".ape") == 0) args->ftype = FORMAT_APE;
		    else if(strcmp(ce, ".mp3") == 0) args->ftype = FORMAT_MP3;
		    else if(strcmp(ce, ".wav") == 0) args->ftype = FORMAT_WAV;
		    else return printf("error: cue references file %s with unsupported extension\n", args->file);
		}
		if(args->track == 0) return 0;
	    } else {
		if(found_track) {
		    for(c = buff; isspace(*c) && *c; c++) ;
		    if(strncmp(c, "INDEX 01 ", 9) == 0) {
		        c += 9;
			ce = strchr(c, ':');
			if(!ce) return printf("cue parse error: invalid track time\n");
			*ce = 0;
			args->min = atoi(c);
			c = ce + 1;
			ce = strchr(c, ':');
			if(!ce) return printf("cue parse error: invalid track time\n");
			*ce = 0;
			args->sec = atoi(c);
			if(title) {
			    if(quiet_run) printf("Track %d: %s\n", args->track, title);	    
			    else printf("Track %d: %s at %d min %02d sec\n", args->track, title, args->min, args->sec);	    
			    free(title);
			} else printf("Found cue track %d for file %s at [%d:%02d]\n", 
					args->track, args->file, args->min, args->sec);
			fclose(f);
			return 0;
		    } else if(strncmp(c, "TITLE ", 6) == 0) {
			ce = c + strlen(c) - 1; 
			while(isspace(*ce)) *ce-- = 0;
			c += 6; title = strdup(c);		
		    } else if(strncmp(c, "TRACK", 5) == 0) return printf("cue parse error: no INDEX 01 section for this track\n");	
		    continue;		
		}
		for(c = buff; isspace(*c) && *c; c++) ;
		if(strncmp(c, track, strlen(track)) == 0) found_track = 1;
	    }	
	}
    return printf("cue parse error: track not found in %s\n", args->file);	
}


