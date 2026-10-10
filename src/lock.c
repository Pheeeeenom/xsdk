#include "lock_private.h"

#include <stdlib.h>
#include <sys/lock.h>

#include "xsdk/kernel.h"

// newlib's locks (built with --enable-newlib-retargetable-locking) on kernel critical sections, which are recursive.

struct __lock {
	RTL_CRITICAL_SECTION section;
};

// The static locks newlib expects the target to define (newlib/libc/misc/lock.c).
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

static struct __lock *const static_locks[] = {
	&__lock___sfp_recursive_mutex,
	&__lock___atexit_recursive_mutex,
	&__lock___at_quick_exit_mutex,
	&__lock___malloc_recursive_mutex,
	&__lock___env_recursive_mutex,
	&__lock___tz_mutex,
	&__lock___dd_hash_mutex,
	&__lock___arc4random_mutex,
};

void xsdk_lock_init(void)
{
	for (size_t i = 0; i < sizeof(static_locks) / sizeof(static_locks[0]); i++)
		RtlInitializeCriticalSection(&static_locks[i]->section);
}

void __retarget_lock_init(_LOCK_T *lock)
{
	*lock = malloc(sizeof(**lock));
	if (*lock)
		RtlInitializeCriticalSection(&(*lock)->section);
}

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
	__retarget_lock_init(lock);
}

void __retarget_lock_close(_LOCK_T lock)
{
	free(lock);
}

void __retarget_lock_close_recursive(_LOCK_T lock)
{
	free(lock);
}

void __retarget_lock_acquire(_LOCK_T lock)
{
	if (lock)
		RtlEnterCriticalSection(&lock->section);
}

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
	__retarget_lock_acquire(lock);
}

// Nonzero when it took the lock, as newlib's own stub reports (newlib/libc/misc/lock.c).
int __retarget_lock_try_acquire(_LOCK_T lock)
{
	return lock && RtlTryEnterCriticalSection(&lock->section);
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
	return __retarget_lock_try_acquire(lock);
}

void __retarget_lock_release(_LOCK_T lock)
{
	if (lock)
		RtlLeaveCriticalSection(&lock->section);
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
	__retarget_lock_release(lock);
}
