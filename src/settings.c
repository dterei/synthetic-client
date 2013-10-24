#include "settings.h"

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GC_THREADS
#include <gc.h>
#define GC_CALLOC(m,n) GC_MALLOC((m)*(n))

// print command line usage.
void usage(void) {
	printf("-h          print this help and exit\n"
	       "-p <num>    TCP port number to listen on (default: 11210)\n"
	       "-t <num>    number of threads to use (default: 4)\n"
			 "-T          set distinct cpu affinity for threads, round-robin\n"
	       "-s <server> memcache server to connect to at backend\n"
			 "-u <users>  number of test users to generate.\n"
			 "-d <num>    mean (normal distribution) of allocation per-request (default: 0)\n"
			 "-D <num>    stdev (normal distribution) of allocation per-request (default: 0)\n"
			 "-r <num>    mean (normal distribution) of response-time (default: 0)\n"
			 "-R <num>    stdev (normal distribution) of response-time (default: 0)\n"
			 "-C <num>    response-time delay cutoff (below this, no delay, default: 0)\n"
	       "-v          verbose (print errors/warnings while in event loop)\n"
	       "-vv         very verbose (also print client commands/reponses)\n"
	       "-vvv        extremely verbose (also print internal state transitions)\n");
	return;
}

// create a new, default settings value.
settings settings_init(void) {
	settings s;
	s.verbose = 0;
	s.threads = 1;
	s.tcpport = 11211;
	s.thread_affinity = false;
	s.users = 0;
	s.backends.len = 0;
	// just allocate a big one so we should never need to expand.
	s.backends.size = 1000;
	s.backends.hosts = GC_MALLOC(sizeof(char*) * s.backends.size);
	s.stats = new_stats();
	s.alloc_mean = 0;
	s.alloc_stddev = 0;
	s.alloc = false;
	s.rtt_mean = 0;
	s.rtt_stddev = 0;
	s.rtt_delay = false;
	s.rtt_cutoff = 0;
	return s;
}

// process arguments
bool settings_parse(int argc, char **argv, settings *s) {
	char c;
	int len;
	char *str;

	while (-1 != (c = getopt(argc, argv,
	       "p:" // TCP port number to listen on
	       "h"  // help, licence info
	       "v"  // verbose
	       "t:" // threads
			 "T"  // cpu-affinity
	       "s:" // backend
			 "u:" // users
			 "d:" // normal distribution mean
			 "D:" // normal distribution stddev
			 "r:" // normal distribution mean
			 "R:" // normal distribution stddev
			 "C:" // normal distribution stddev
		))) {
		switch (c) {
		case 'p':
			s->tcpport = atoi(optarg);
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		case 'v':
			s->verbose++;
			break;
		case 't':
			s->threads = atoi(optarg);
			if (s->threads <= 0) {
				fprintf(stderr, "Number of threads must be greater than 0\n");
				return false;
			}
			break;
		case 'T':
			s->thread_affinity = true;
			break;
		case 's':
			len = strnlen(optarg, MAX_SERVER_STRING) + 1;
			str = GC_MALLOC(sizeof(char) * len);
			strncpy(str, optarg, len);
			str[len - 1] = '\0';
			s->backends.hosts[s->backends.len] = str;
			s->backends.len++;
			break;
		case 'u':
			s->users = atoi(optarg);
			if (s->users < 0) {
				fprintf(stderr, "Number of users must be at least 0\n");
				return false;
			}
			break;
		case 'd':
			s->alloc = true;
			s->alloc_mean = atof(optarg);
			break;
		case 'D':
			s->alloc = true;
			s->alloc_stddev = atof(optarg);
			break;
		case 'r':
			s->rtt_delay = true;
			s->rtt_mean = atof(optarg);
			break;
		case 'R':
			s->rtt_delay = true;
			s->rtt_stddev = atof(optarg);
			break;
		case 'C':
			s->rtt_cutoff = atof(optarg);
			break;
		default:
			fprintf(stderr, "Illegal argument \"%c\"\n", c);
			return false;
		}
	}

	/* if (s->backends.len <= 0) { */
	/* 	errx(1, "Must include at least one memcached RPC backend!\n"); */
	/* } */
	return true;
}

