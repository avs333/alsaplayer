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

int quiet_run = 0;

static playback_ctx *ctx = 0;

struct call_args {
    int min, sec, ftype; 	
    char *file;	
    int track;
};

void bye(int sig) 
{
    printf("exiting on signal %d\n", sig);
    if(ctx) audio_exit(0, 0, ctx);	    
    exit(0);
}

static int usage(char *prog) 
{
   return printf("Usage: %s [-c card] [-d device] [-s min:sec | -t track_no] [-q] <file>\n", prog);
}

void *thd(void *a) 
{
    struct call_args *args = (struct call_args *) a;
    sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	audio_play(0, 0, ctx, args->file, args->ftype, args->min*60 + args->sec);	
    return 0;	
}

void pause_resume(int sig) {
    if(sig == SIGUSR1) audio_pause(0,0,ctx);
    else audio_resume(0,0,ctx);
}

static int parse_cue(struct call_args *args);

int main(int argc, char **argv)
{
    int card = 0, device = 0, opt;
    char *c;
    pthread_t thread;
    sigset_t set;
    struct call_args args[1];

	memset(args, 0, sizeof(struct call_args));
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);

	signal(SIGINT, bye);
	signal(SIGUSR1, pause_resume);	
	signal(SIGUSR2, pause_resume);	

	while ((opt = getopt(argc, argv, "c:d:s:t:q")) != -1) {
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
		case 't':
		    args->track = atoi(optarg);
		    break; 
		case 'q':
		    quiet_run = 1;
		    break; 	
		default: /* '?' */
		    return usage(argv[0]);
	    }
	}
	if(optind >= argc) return usage(argv[0]);

	args->file = strdup(argv[optind]);
	c = strrchr(args->file, '.');
	if(c) {
	    if(strcmp(c, ".flac") == 0) {
		args->ftype = FORMAT_FLAC;
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".ape") == 0) {
		args->ftype = FORMAT_APE;	
		if(args->track) printf("not a cue file, track specification ignored\n");
	    } else if(strcmp(c, ".cue") == 0) {
		if(args->min || args->sec) printf("start time specificaton ignored for cue file\n");
		if(parse_cue(args) != 0) {
		    if(args->file) free(args->file);	
		    return -1;
		}
	    }
	}

	if(args->ftype == 0) return printf("file extension must be .flac or .ape\n");

	ctx = (playback_ctx *) audio_init(0, 0, 0, card, device);
	if(!ctx) return -1;	

	pthread_create(&thread, 0, thd, args);
	pthread_join(thread, 0);

	audio_exit(0, 0, ctx);
	if(args->file) free(args->file);

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
		if(strncmp(c+1, "WAVE", 4) != 0 && strncmp(c+1, "FLAC", 4) != 0 
		   && strncmp(c+1, "APE", 3) != 0) continue;
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
		    *ce = 0;
		    strcat(args->file, ".flac");
		    if(stat(args->file, &st) == 0) args->ftype = FORMAT_FLAC;
		    else {
			*ce = 0;
			strcat(args->file, ".ape");
			if(stat(args->file, &st) == 0) args->ftype = FORMAT_APE;
			else {
			    *ce = 0;
			    return printf("error: cue references non-existing file %s\n", args->file);
			}
		    }
		} else {
		    if(strcmp(ce, ".flac") == 0) args->ftype = FORMAT_FLAC;
		    else if(strcmp(ce, ".ape") == 0) args->ftype = FORMAT_APE;
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


