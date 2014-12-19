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
#include "flac/decoder.h"
#include "main.h"

#define _BSD_SOURCE	1
#include <features.h>
#include <sys/time.h>
#define __USE_BSD  1
#include <getopt.h>

static playback_ctx *ctx = 0;

struct call_args {
    int min, sec, ftype; 	
    char *file;	
};

void bye(int sig) 
{
    printf("exiting on signal %d\n", sig);
    if(ctx) audio_exit(0, 0, ctx);	    
    exit(0);
}

static int usage(char *prog) 
{
   return printf("Usage: %s [-c card] [-d device] [-s min:sec] file.flac\n", prog);
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

	while ((opt = getopt(argc, argv, "c:d:s:")) != -1) {
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
		default: /* '?' */
		   return usage(argv[0]);
	    }
	}
	if(optind >= argc) return usage(argv[0]);

	args->file = argv[optind];
	c = strrchr(args->file, '.');
	if(c) {
	    if(strcmp(c, ".flac") == 0) args->ftype = FORMAT_FLAC;
	    else if(strcmp(c, ".ape") == 0) args->ftype = FORMAT_APE;	
	}
	if(args->ftype == 0) return printf("file extension must be .flac or .ape\n");

	ctx = (playback_ctx *) audio_init(0, 0, 0, card, device);
	if(!ctx) return 1;	

	pthread_create(&thread, 0, thd, args);
	pthread_join(thread, 0);

	audio_exit(0, 0, ctx);

    return 0;
}


