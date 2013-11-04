#ifndef _STATS_H
#define _STATS_H

#define STATS_HASH_MAP_SIZE 100

#include <pthread.h>
#include <memory>

typedef struct _client_stats {
	pthread_mutex_t lock;
	unsigned short refcnt;
	struct _client_stats *next;
	int client_id;
	int total_connections;
	int live_connections;
	int requests;
} client_stats;

typedef struct _stats {
	pthread_mutex_t lock;
	int clients;
	int map_size;
	client_stats **map;
} statistics;

statistics *new_stats(int);
std::shared_ptr<client_stats> get_client_stats(statistics *s, int client_id);
void rm_client_stats(statistics *s, int client_id);
void free_client_stats(std::shared_ptr<client_stats> cs);
void stat_test_data(statistics *stats, int amount);

#endif
