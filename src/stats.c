#include "locking.h"
#include "server.h"
#include "stats.h"

#include <pthread.h>
#include <stdlib.h>

#define GC_THREADS
#include <gc.h>
#define GC_CALLOC(m,n) GC_MALLOC((m)*(n))

// create a new statistics value.
statistics *new_stats(int size) {
	statistics *s = RC_MALLOC(sizeof(statistics));
	pthread_mutex_init(&s->lock, NULL);
	s->refcnt = 1;
	s->clients = 0;
	s->map_size = size;
	s->map = RC_CALLOC(sizeof(client_stats*), size);
	return s;
}

// get the stats for a particular client (will create a new client_stats value
// if needed).
client_stats *get_client_stats(statistics *s, int client_id) {
	int key = client_id % s->map_size;
	client_stats *cs = s->map[key];
	
	while (cs != NULL && cs->client_id != client_id) {
		cs = cs->next;
	}

	if (cs == NULL) {
		cs = RC_CALLOC(sizeof(client_stats), 1);
		pthread_mutex_init(&cs->lock, NULL);
		cs->client_id = client_id;
		cs->refcnt = 2; // 1 for hashmap ref, one for return ref.
		cs->next = s->map[key];
		s->map[key] = cs;
	} else {
		refcount_incr(&cs->refcnt, &cs->lock);
	}

	return cs;
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
	
	// decr refcnt and maybe free.
	if (refcount_decr(&cs->refcnt, &cs->lock) == 0) {
		free_client_stats(cs);
	}
}

// free a client stats structure.
void free_client_stats(client_stats *cs) {
	pthread_mutex_destroy(&cs->lock);
	/* free(cs); */
}

// generate some test client data in stats.
void stat_test_data(statistics *stats, int amount) {
	if (config.verbose > 0) {
		fprintf(stderr, "Generating %d test users\n", amount);
	}

	for (int curr = 0; curr < amount; curr++) {
		int client_id = rand();
		client_stats *cs = get_client_stats(stats, client_id);
		refcount_decr(&cs->refcnt, &cs->lock);
	}
}

