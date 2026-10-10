#include "reent_private.h"

#include <pthread.h>
#include <reent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "xsdk/log.h"

// newlib (built with __DYNAMIC_REENT__) reaches each thread's errno and stdio state through __getreent.

static pthread_key_t reent_key;
static bool reent_key_ready;

static void free_reent(void *value)
{
	_reclaim_reent(value);
	_free_r(_impure_ptr, value);
}

void xsdk_reent_init(void)
{
	if (pthread_key_create(&reent_key, free_reent) != 0) {
		xsdk_log_write("reent: pthread_key_create failed; threads will share errno");
		return;
	}
	// The main thread keeps newlib's static state.
	(void)pthread_setspecific(reent_key, _impure_ptr);
	reent_key_ready = true;
}

struct _reent *__getreent(void)
{
	if (!reent_key_ready)
		return _impure_ptr;
	struct _reent *reent = pthread_getspecific(reent_key);
	if (reent)
		return reent;
	// Allocate against the main thread's state; plain malloc would ask for this thread's, and recurse.
	reent = _malloc_r(_impure_ptr, sizeof(*reent));
	if (!reent)
		return _impure_ptr;
	_REENT_INIT_PTR(reent);
	(void)pthread_setspecific(reent_key, reent);
	return reent;
}
