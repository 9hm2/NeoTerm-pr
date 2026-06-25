/* uKernel fake <pthread.h> for the Android/aarch64 (LP64) FS engine build.
 *
 * Provides ABI-correct pthread types + the function declarations the sync shim
 * uses, but NOTHING ELSE — crucially NOT bionic's real <pthread.h> nor
 * <sys/types.h>, whose cascade (time.h/signal.h, __kernel_fsid_t, ...) drags
 * real kernel-uapi definitions into the kernel-FS translation units and
 * collides with uKernel's fake kernel headers. The struct layouts below are
 * copied verbatim from bionic bits/pthread_types.h (LP64 branch), so the
 * objects are link-compatible with libc's real pthread implementation. */
#ifndef _UK_PTHREAD_H
#define _UK_PTHREAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* pthread TYPES come from bionic <sys/types.h> (pulled by linux/types.h);
 * we only shadow the FUNCTION cascade (time.h/signal.h) here. */
#include <sys/types.h>

#define PTHREAD_MUTEX_INITIALIZER  {{0}}
#define PTHREAD_COND_INITIALIZER   {{0}}
#define PTHREAD_RWLOCK_INITIALIZER {{0}}
#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    0
extern int pthread_mutexattr_init(pthread_mutexattr_t*);
extern int pthread_mutexattr_settype(pthread_mutexattr_t*, int);
extern int pthread_mutexattr_destroy(pthread_mutexattr_t*);

extern int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
extern int pthread_mutex_lock(pthread_mutex_t*);
extern int pthread_mutex_trylock(pthread_mutex_t*);
extern int pthread_mutex_unlock(pthread_mutex_t*);
extern int pthread_mutex_destroy(pthread_mutex_t*);
extern int pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
extern int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern int pthread_cond_signal(pthread_cond_t*);
extern int pthread_cond_broadcast(pthread_cond_t*);
extern int pthread_cond_destroy(pthread_cond_t*);
extern int pthread_rwlock_init(pthread_rwlock_t*, const pthread_rwlockattr_t*);
extern int pthread_rwlock_rdlock(pthread_rwlock_t*);
extern int pthread_rwlock_wrlock(pthread_rwlock_t*);
extern int pthread_rwlock_unlock(pthread_rwlock_t*);
extern int pthread_rwlock_destroy(pthread_rwlock_t*);
extern int pthread_create(pthread_t*, const void*, void*(*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern int pthread_detach(pthread_t);
extern pthread_t pthread_self(void);
extern int pthread_key_create(pthread_key_t*, void(*)(void*));
extern int pthread_key_delete(pthread_key_t);
extern void* pthread_getspecific(pthread_key_t);
extern int pthread_setspecific(pthread_key_t, const void*);

#ifdef __cplusplus
}
#endif
#endif
