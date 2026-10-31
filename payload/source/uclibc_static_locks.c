/* Real locks for libc's internal mutexes in static uClibc-ng builds.
 *
 * The Bootlin 2017.05 toolchains ship uClibc-ng 1.0.24 with libpthread merged into libc.a.
 * libc.a defines __pthread_mutex_lock and friends twice, both weak: as no-op stubs in
 * __uClibc_main.o and as the real functions in pthread_mutex_lock.o. A static link keeps
 * the first one it sees, the stub, so malloc, free, stdio and the time functions run
 * without locks and threads corrupt the heap (malloc then aborts or crashes). These
 * strong definitions take precedence over both and route libc to the real mutexes.
 *
 * Link this file into every threaded binary built with those toolchains. Other C
 * libraries resolve these names themselves, so the file is empty there. */
#include <pthread.h> /* defines __UCLIBC__ through features.h */

#ifdef __UCLIBC__
int __pthread_mutex_lock(pthread_mutex_t *m) { return pthread_mutex_lock(m); }
int __pthread_mutex_unlock(pthread_mutex_t *m) { return pthread_mutex_unlock(m); }
int __pthread_mutex_trylock(pthread_mutex_t *m) { return pthread_mutex_trylock(m); }
int __pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { return pthread_mutex_init(m, a); }
#endif
