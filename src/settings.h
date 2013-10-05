// Server settings and option parsing
#ifndef _SETTINGS_H
#define _SETTINGS_H

#include <gsl/gsl_rng.h>
#include <stdbool.h>

#define MAX_SERVER_STRING 300

typedef struct _backends {
	char **hosts;
	int len;
	int size;
} backends;

// Settings the server has
typedef struct _settings {
	int verbose;
	int threads;
	int tcpport;
	double dist_arg1; // normal distribution mean
	double dist_arg2; // normal distribution stddev
	bool use_dist;
	gsl_rng *r;
	backends backends;
} settings;

void usage(void);
settings settings_init(void);
bool settings_parse(int argc, char **argv, settings *s);

#endif

