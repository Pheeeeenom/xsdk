// newlib declares CLOCK_MONOTONIC_RAW only to GNU code.
#define _GNU_SOURCE

#include "time_private.h"

#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "xsdk/kernel.h"

// POSIX clocks: CLOCK_REALTIME from the kernel's system time, which steps a millisecond at a time on the console;
// CLOCK_MONOTONIC and CLOCK_MONOTONIC_RAW from the CPU's time base, which nothing adjusts.

// 100 ns intervals from 1601, where NT time starts, to 1970.
static const int64_t NT_UNIX_EPOCH = 116444736000000000LL;

enum {
	NT_TICKS_PER_SECOND = 10000000,
	NANOSECONDS_PER_SECOND = 1000000000,
	NANOSECONDS_PER_NT_TICK = 100,
	// The CPU's 3192 MHz over 64 (libxenon:libxenon/drivers/ppc/timebase.h:6). KeQueryPerformanceFrequency reports
	// 50 MHz (results/2026-10-10-probe-vblank/xsdk.log:3), which reads the console's 60 Hz vblank 0.25% short
	// (docs/HARDWARE.md, "The time base runs at 49.875 MHz"). Xenia runs it at 50 MHz (xenia:src/xenia/emulator.cc:151).
	TIME_BASE_HZ = 49875000,
};

static uint64_t time_base(void)
{
	uint32_t upper;
	uint32_t lower;
	uint32_t check;
	// The upper half can tick between the two reads in 32-bit mode.
	do {
		__asm__ volatile("mftbu %0" : "=r"(upper));
		__asm__ volatile("mftb %0" : "=r"(lower));
		__asm__ volatile("mftbu %0" : "=r"(check));
	} while (upper != check);
	return (uint64_t)upper << 32 | lower;
}

struct timespec xsdk_time_from_nt(uint64_t nt_time)
{
	const int64_t since_1970 = (int64_t)nt_time - NT_UNIX_EPOCH;
	struct timespec time = {
		.tv_sec = since_1970 / NT_TICKS_PER_SECOND,
		.tv_nsec = (long)(since_1970 % NT_TICKS_PER_SECOND) * NANOSECONDS_PER_NT_TICK,
	};
	// Division rounds toward zero; times before 1970 still need a positive tv_nsec.
	if (time.tv_nsec < 0) {
		time.tv_sec--;
		time.tv_nsec += NANOSECONDS_PER_SECOND;
	}
	return time;
}

int clock_gettime(clockid_t clock, struct timespec *time)
{
	if (clock == CLOCK_REALTIME) {
		uint64_t now;
		KeQuerySystemTime(&now);
		*time = xsdk_time_from_nt(now);
		return 0;
	}
	if (clock == CLOCK_MONOTONIC || clock == CLOCK_MONOTONIC_RAW) {
		const uint64_t ticks = time_base();
		time->tv_sec = (time_t)(ticks / TIME_BASE_HZ);
		time->tv_nsec = (long)(ticks % TIME_BASE_HZ * NANOSECONDS_PER_SECOND / TIME_BASE_HZ);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int gettimeofday(struct timeval *time, void *zone)
{
	(void)zone;
	struct timespec now;
	(void)clock_gettime(CLOCK_REALTIME, &now);
	time->tv_sec = now.tv_sec;
	time->tv_usec = now.tv_nsec / 1000;
	return 0;
}

static int64_t monotonic_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * NANOSECONDS_PER_SECOND + now.tv_nsec;
}

int nanosleep(const struct timespec *request, struct timespec *remaining)
{
	if (request->tv_nsec < 0 || request->tv_nsec >= NANOSECONDS_PER_SECOND || request->tv_sec < 0) {
		errno = EINVAL;
		return -1;
	}
	// The kernel's relative wait ends on its clock tick, up to a tick before the time asked for (docs/HARDWARE.md,
	// "The time base runs at 49.875 MHz"), so wait again for what's left until CLOCK_MONOTONIC passes the deadline.
	const int64_t start = monotonic_ns();
	// A sleep past what the clock can count is as good as forever.
	const int64_t deadline = request->tv_sec < (INT64_MAX - start) / NANOSECONDS_PER_SECOND
		? start + (int64_t)request->tv_sec * NANOSECONDS_PER_SECOND + request->tv_nsec
		: INT64_MAX;
	for (int64_t left = deadline - start; left > 0; left = deadline - monotonic_ns()) {
		// A negative interval is relative.
		const int64_t interval = -((left + NANOSECONDS_PER_NT_TICK - 1) / NANOSECONDS_PER_NT_TICK);
		(void)KeDelayExecutionThread(USER_MODE, 0, &interval);
	}
	if (remaining) {
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}
