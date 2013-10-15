#include "stats.h"

#include <pthread.h>
#include <stdlib.h>

// create a new statistics value.
statistics *new_stats(void) {
	statistics *s = malloc(sizeof(statistics));
	pthread_mutex_init(&s->lock, NULL);
	s->refcnt = 1;
	s->clients = 0;
	s->map_size = STATS_HASH_MAP_SIZE;
	s->map = calloc(sizeof(client_stats*), STATS_HASH_MAP_SIZE);
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
		cs = calloc(sizeof(client_stats), 1);
		pthread_mutex_init(&cs->lock, NULL);
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
	free(cs);
}

// increase the refcnt of an object.
inline unsigned short refcount_incr(unsigned short *refcount, pthread_mutex_t *mutex) {
#ifdef HAVE_GCC_ATOMICS
	return __sync_add_and_fetch(refcount, 1);
#else
	unsigned short res;
	mutex_lock(mutex);
	(*refcount)++;
	res = *refcount;
	mutex_unlock(mutex);
	return res;
#endif
}

// decrease the refcnt of an object.
inline unsigned short refcount_decr(unsigned short *refcount, pthread_mutex_t *mutex) {
#ifdef HAVE_GCC_ATOMICS
	return __sync_sub_and_fetch(refcount, 1);
#else
	unsigned short res;
	mutex_lock(mutex);
	(*refcount)--;
	res = *refcount;
	mutex_unlock(mutex);
	return res;
#endif
}

// lock a mutex (spin lock).
inline int mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex));
    return 0;
}

