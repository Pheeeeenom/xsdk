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
};

// Read once; the kernel reports the time base's rate (SCOPE.md section 2.5).
static uint32_t time_base_hz;

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
		if (!time_base_hz)
			time_base_hz = KeQueryPerformanceFrequency();
		const uint64_t ticks = time_base();
		time->tv_sec = (time_t)(ticks / time_base_hz);
		time->tv_nsec = (long)(ticks % time_base_hz * NANOSECONDS_PER_SECOND / time_base_hz);
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

int nanosleep(const struct timespec *request, struct timespec *remaining)
{
	if (request->tv_nsec < 0 || request->tv_nsec >= NANOSECONDS_PER_SECOND || request->tv_sec < 0) {
		errno = EINVAL;
		return -1;
	}
	// A negative interval is relative; round up so the sleep is never short.
	const int64_t interval = -((int64_t)request->tv_sec * NT_TICKS_PER_SECOND +
		(request->tv_nsec + NANOSECONDS_PER_NT_TICK - 1) / NANOSECONDS_PER_NT_TICK);
	(void)KeDelayExecutionThread(USER_MODE, 0, &interval);
	if (remaining) {
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}
