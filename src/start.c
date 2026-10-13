#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "lock_private.h"
#include "microcode_private.h"
#include "pthread_private.h"
#include "reent_private.h"
#include "xsdk/log.h"

int main(int argc, char **argv);
void xsdk_start(void);

// Bounds that src/xsdk.ld defines.
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern void (*__fini_array_start[])(void);
extern void (*__fini_array_end[])(void);
extern const char __eh_frame_start[];

// libgcc's unwinder (unwind-dw2-fde.h); with no ELF loader to ask, the title registers its frames itself.
void __register_frame_info(const void *begin, void *object);

// Room for libgcc's struct object, which is seven words today; crtbegin.o would normally provide it.
static uint32_t eh_frame_object[16];

// __cxa_atexit names the image by this; crtbegin.o would normally provide it.
void *__dso_handle = &__dso_handle;

static void run_fini_array(void)
{
	for (size_t i = (size_t)(__fini_array_end - __fini_array_start); i > 0; i--)
		__fini_array_start[i - 1]();
}

void xsdk_start(void)
{
	xsdk_lock_init();
	xsdk_microcode_init();
	xsdk_pthread_init();
	xsdk_reent_init();
	(void)xsdk_log_open();
	__register_frame_info(__eh_frame_start, eh_frame_object);

	for (void (**function)(void) = __preinit_array_start; function < __preinit_array_end; function++)
		(*function)();
	for (void (**function)(void) = __init_array_start; function < __init_array_end; function++)
		(*function)();
	// Registered first, so it runs after the static destructors the constructors registered.
	(void)atexit(run_fini_array);

	// argv[0] empty is C's "no program name" (C11 5.1.2.2.1); desktop code reads its arguments from argv[1].
	static char program_name[] = "";
	char *argv[] = { program_name, NULL };
	exit(main(1, argv));
}
