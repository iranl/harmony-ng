/* Condition variables on CLOCK_MONOTONIC, for timed waits that a clock step (hub_sntp sets the
 * clock long after boot) does not stretch or cut short. */
#ifndef MONO_COND_H
#define MONO_COND_H

#include <pthread.h>
#include <time.h>

static inline int mono_cond_init(pthread_cond_t *c) {
    pthread_condattr_t a;
    int rc;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    rc = pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
    return rc;
}

/* the CLOCK_MONOTONIC time ms from now, for pthread_cond_timedwait */
static inline struct timespec mono_deadline(long ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

#endif
