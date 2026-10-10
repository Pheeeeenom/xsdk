#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// The title is the only process, with this id.
enum { TITLE_PID = 1 };

// newlib's exit() ends here after running atexit handlers and static destructors.
void _exit(int status)
{
	xsdk_log_write("exit: status %d", status);
	xsdk_log_close();
	XamLoaderTerminateTitle();
	// _exit must not return, even if XamLoaderTerminateTitle ever does.
	for (;;)
		(void)xsdk_thread_sleep(1000);
}

pid_t getpid(void)
{
	return TITLE_PID;
}

// abort() raises SIGABRT through kill; with no handler, end the title as a signal would.
int kill(pid_t pid, int signal)
{
	if (pid != TITLE_PID) {
		errno = ESRCH;
		return -1;
	}
	xsdk_log_write("exit: signal %d", signal);
	_exit(128 + signal);
}
