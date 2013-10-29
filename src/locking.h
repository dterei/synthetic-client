#ifndef _LOCKING_H
#define _LOCKING_H

#include <pthread.h>

pthread_mutex_t refcnt_lock;;
void locking_init(void);

int mutex_lock(pthread_mutex_t *mutex);
#define mutex_unlock(x) pthread_mutex_unlock(x)
unsigned short refcount_incr(unsigned short *refcount, pthread_mutex_t *mutex);
unsigned short refcount_decr(unsigned short *refcount, pthread_mutex_t *mutex);

#endif
