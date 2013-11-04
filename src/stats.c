#include "locking.h"
#include "server.h"
#include "stats.h"

#include <pthread.h>
#include <stdlib.h>

#include <memory>

#define GC_THREADS
#include <gc.h>
#define GC_CALLOC(m,n) GC_MALLOC((m)*(n))

struct D {    // a verbose array deleter:
	void operator()(client_stats* stats) {
		if (config.verbose > 1) {
			fprintf(stderr, "[deleter called: %d]\n", stats->client_id);
		}
		// delete[] p;
	}
};

// create a new statistics value.
statistics *new_stats(int size) {
	statistics *s = (statistics *) RC_MALLOC(sizeof(statistics));
	pthread_mutex_init(&s->lock, NULL);
	s->clients = 0;
	s->map_size = size;
	s->map = (client_stats **) RC_CALLOC(sizeof(client_stats*), size);
	return s;
}

// get the stats for a particular client (will create a new client_stats value
// if needed).
std::shared_ptr<client_stats> get_client_stats(statistics *s, int client_id) {
	int key = client_id % s->map_size;
	client_stats *cs = s->map[key];
	
	while (cs != NULL && cs->client_id != client_id) {
		cs = cs->next;
	}

	if (cs == NULL) {
		cs = (client_stats *) RC_CALLOC(sizeof(client_stats), 1);
		pthread_mutex_init(&cs->lock, NULL);
		cs->client_id = client_id;
		cs->next = s->map[key];
		s->map[key] = cs;
	}

	if (config.verbose > 1) {
		fprintf(stderr, "[get_client_stats: %d]\n", cs->client_id);
	}

	std::shared_ptr<client_stats> shared_cs (cs, D());

	return shared_cs;
}

// remove a clients stats structure.
void rm_client_stats(statistics *s, int client_id) {
	int key = client_id % s->map_size;
	client_stats *prev = NULL;
	client_stats *cs = s->map[key];

	while (cs != NULL && cs->client_id != client_id) {
		prev = cs;
		cs = cs->next;
	}

	// remove from hashmap
	if (cs == NULL) {
		return;
	} else if (prev == NULL) {
		s->map[key] = NULL;
	} else {
		prev->next = cs->next;
	}

	if (config.verbose > 1) {
		fprintf(stderr, "[rm_client_stats: %d]\n", cs->client_id);
	}
}

// free a client stats structure.
void free_client_stats(std::shared_ptr<client_stats> cs) {
	if (config.verbose > 1) {
		fprintf(stderr, "[free client stats: %d]\n", cs->client_id);
	}
	/* pthread_mutex_destroy(&cs->lock); */
	/* free(cs); */
}

// generate some test client data in stats.
void stat_test_data(statistics *stats, int amount) {
	if (config.verbose > 0) {
		fprintf(stderr, "Generating %d test users\n", amount);
	}

	for (int curr = 0; curr < amount; curr++) {
		int client_id = rand();
		std::shared_ptr<client_stats> cs = get_client_stats(stats, client_id);
	}
}

