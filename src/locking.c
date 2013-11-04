#include "locking.h"
#include <pthread.h>
#include <stdlib.h>

// increase the refcnt of an object.
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

// lock a mutex (spin lock).
int mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex));
    return 0;
}

