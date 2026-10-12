// newlib declares CLOCK_MONOTONIC_RAW only to GNU code.
#define _GNU_SOURCE

#include "tests.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/log.h"

enum {
	NANOSECONDS_PER_SECOND = 1000000000,
	// The time base runs at about 50 MHz; the kernel's system time, which a refused clock falls back to in SDL,
	// steps a millisecond at a time on the console.
	FINEST_STEP_NS = 100000,
	// Enough reads to see a millisecond clock step, so a clock that never moves can't hang the test.
	STEP_READS = 1000000,
};

static int64_t nanoseconds(const struct timespec *time)
{
	return (int64_t)time->tv_sec * NANOSECONDS_PER_SECOND + time->tv_nsec;
}

// The first change a clock shows when read over and over, or -1 if it can't be read or never moves.
static int64_t clock_step_ns(clockid_t clock)
{
	struct timespec time;
	if (clock_gettime(clock, &time) != 0)
		return -1;
	const int64_t first = nanoseconds(&time);
	for (int i = 0; i < STEP_READS; i++) {
		if (clock_gettime(clock, &time) != 0)
			return -1;
		if (nanoseconds(&time) != first)
			return nanoseconds(&time) - first;
	}
	return -1;
}

void test_clocks(void)
{
	// SDL's timer reads CLOCK_MONOTONIC_RAW wherever the C library defines it, as newlib does.
	struct timespec before;
	struct timespec raw;
	struct timespec after;
	const bool read = clock_gettime(CLOCK_MONOTONIC, &before) == 0 && clock_gettime(CLOCK_MONOTONIC_RAW, &raw) == 0 &&
		clock_gettime(CLOCK_MONOTONIC, &after) == 0;
	check("clocks: CLOCK_MONOTONIC_RAW reads the time base CLOCK_MONOTONIC does",
		read && nanoseconds(&before) <= nanoseconds(&raw) && nanoseconds(&raw) <= nanoseconds(&after));

	const int64_t monotonic_step = clock_step_ns(CLOCK_MONOTONIC);
	const int64_t raw_step = clock_step_ns(CLOCK_MONOTONIC_RAW);
	const int64_t realtime_step = clock_step_ns(CLOCK_REALTIME);
	xsdk_log_write("clocks: steps of %lld ns monotonic, %lld ns raw, %lld ns realtime", (long long)monotonic_step,
		(long long)raw_step, (long long)realtime_step);
	check("clocks: CLOCK_MONOTONIC and CLOCK_MONOTONIC_RAW step finer than 0.1 ms",
		monotonic_step > 0 && monotonic_step < FINEST_STEP_NS && raw_step > 0 && raw_step < FINEST_STEP_NS);
}
