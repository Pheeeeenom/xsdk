#include <stddef.h>

#include "malloc_lock.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

int main(int argc, char **argv);
void xsdk_start(void);

void xsdk_start(void)
{
	char *argv[] = { NULL };
	xsdk_malloc_lock_init();
	(void)xsdk_log_open();
	int exit_code = main(0, argv);
	xsdk_log_write("exit: main returned %d", exit_code);
	xsdk_log_close();
	XamLoaderTerminateTitle();
	xsdk_log_write("exit: XamLoaderTerminateTitle returned");
}
