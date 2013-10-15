#ifndef _STATS_H
#define _STATS_H

#define STATS_HASH_MAP_SIZE 1000000

#include <pthread.h>

typedef struct _client_stats {
	pthread_mutex_t lock;
	short refcnt;
	struct _client_stats *next;
	int client_id;
	int total_connections;
	int live_connections;
	int requests;
} client_stats;

typedef struct _stats {
	pthread_mutex_t lock;
	short refcnt;
	int clients;
	int map_size;
	client_stats **map;
} statistics;

statistics *new_stats(void);
client_stats *get_client_stats(statistics *s, int client_id);
void rm_client_stats(statistics *s, int client_id);
void free_client_stats(client_stats *cs);

int mutex_lock(pthread_mutex_t *mutex);
#define mutex_unlock(x) pthread_mutex_unlock(x)
unsigned short refcount_incr(unsigned short *refcount, pthread_mutex_t *mutex);
unsigned short refcount_decr(unsigned short *refcount, pthread_mutex_t *mutex);

#endif
