#ifndef _LOCKING_H
#define _LOCKING_H

#include <pthread.h>
#include <stdlib.h>

// lock a mutex (spin lock).
static inline
int mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex));
    return 0;
}

#define mutex_unlock(x) pthread_mutex_unlock(x)

// increase the refcnt of an object.
static inline
unsigned short refcount_incr(unsigned short *refcount, pthread_mutex_t *mutex) {
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
static inline
unsigned short refcount_decr(unsigned short *refcount, pthread_mutex_t *mutex) {
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

#endif
