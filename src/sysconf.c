#include <errno.h>
#include <unistd.h>

// Three cores with two hardware threads each; Xenia spreads threads over six (xenia:src/xenia/kernel/xthread.cc:162).
enum { HARDWARE_THREADS = 6 };

long sysconf(int name)
{
	switch (name) {
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		return HARDWARE_THREADS;
	default:
		errno = EINVAL;
		return -1;
	}
}
