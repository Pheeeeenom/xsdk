#include "malloc_lock.h"

#include <reent.h>

#include "xsdk/kernel.h"

void __malloc_lock(struct _reent *reent);
void __malloc_unlock(struct _reent *reent);

// newlib's malloc calls these around every heap change; critical sections are recursive, as newlib requires.
static RTL_CRITICAL_SECTION malloc_lock;

void xsdk_malloc_lock_init(void)
{
	RtlInitializeCriticalSection(&malloc_lock);
}

void __malloc_lock(struct _reent *reent)
{
	(void)reent;
	RtlEnterCriticalSection(&malloc_lock);
}

void __malloc_unlock(struct _reent *reent)
{
	(void)reent;
	RtlLeaveCriticalSection(&malloc_lock);
}
