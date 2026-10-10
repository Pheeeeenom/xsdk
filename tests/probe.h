#ifndef XSDK_PROBE_H
#define XSDK_PROBE_H

#include <stdint.h>

#include "xsdk/kernel.h"

void xsdk_start(void);

// About 10 seconds if the time base runs at 50 MHz; the real rate is unverified, so this only needs to be visibly long.
enum { PROBE_SPIN_TICKS = 500000000 };

// Keeps every import the test title uses in the image, so the loader resolves the same set.
__attribute__((used)) static void *const probe_imports[] = {
	(void *)DbgPrint,
	(void *)NtAllocateVirtualMemory,
	(void *)NtClose,
	(void *)NtCreateFile,
	(void *)NtWriteFile,
	(void *)XamLoaderTerminateTitle,
};

static inline uint32_t probe_time_base(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

static inline void probe_spin(void)
{
	const uint32_t start = probe_time_base();
	while (probe_time_base() - start < PROBE_SPIN_TICKS) {
	}
}

#endif
