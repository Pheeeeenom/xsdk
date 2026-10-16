#include <errno.h>
#include <unistd.h>

#include "xsdk/kernel.h"

enum {
	// Three cores with two hardware threads each; Xenia spreads threads over six
	// (xenia:src/xenia/kernel/xthread.cc:162).
	HARDWARE_THREADS = 6,
	// sbrk's pages, and the ones MmQueryStatistics counts (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:496).
	PAGE_BYTES = 4096,
};

// The console reports 131072 pages, its 512 MB (results/2026-10-12-memory/xsdk_test.log).
static long physical_pages(void)
{
	MM_QUERY_STATISTICS_RESULT statistics = { .size = sizeof(statistics) };
	if (!NT_SUCCESS(MmQueryStatistics(&statistics))) {
		errno = EINVAL;
		return -1;
	}
	return statistics.total_physical_pages;
}

long sysconf(int name)
{
	switch (name) {
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		return HARDWARE_THREADS;
	case _SC_PAGESIZE:
		return PAGE_BYTES;
	case _SC_PHYS_PAGES:
		return physical_pages();
	default:
		errno = EINVAL;
		return -1;
	}
}
