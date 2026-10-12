// newlib declares CLOCK_MONOTONIC_RAW only to GNU code.
#define _GNU_SOURCE

#include "tests.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	NANOSECONDS_PER_SECOND = 1000000000,
	// The time base steps every 20 ns; the kernel's system time, which a refused clock falls back to in SDL, a
	// millisecond at a time on the console.
	FINEST_STEP_NS = 100000,
	// Enough reads to see a millisecond clock step, so a clock that never moves can't hang the test.
	STEP_READS = 1000000,
	// Long enough that the kernel's millisecond steps measure the rate to a few parts per million.
	RATE_SPAN_MS = 250,
	// The kernel reports the time base at 50 MHz; it runs at 49.875 MHz, 2500 parts per million slower.
	RATE_TOLERANCE_PPM = 500,
	SLEEP_COUNT = 20,
	SLEEP_NS = 2000000,
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

// Spins until CLOCK_REALTIME next steps, and reads both clocks as it does, so a span measured between two steps has
// no part of a step in it.
static bool wait_realtime_step(int64_t *realtime_ns, int64_t *monotonic_ns)
{
	struct timespec time;
	if (clock_gettime(CLOCK_REALTIME, &time) != 0)
		return false;
	const int64_t first = nanoseconds(&time);
	for (int i = 0; i < STEP_READS; i++) {
		struct timespec monotonic;
		if (clock_gettime(CLOCK_REALTIME, &time) != 0 || clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0)
			return false;
		if (nanoseconds(&time) != first) {
			*realtime_ns = nanoseconds(&time);
			*monotonic_ns = nanoseconds(&monotonic);
			return true;
		}
	}
	return false;
}

static void check_rate(void)
{
	int64_t realtime_start = 0;
	int64_t monotonic_start = 0;
	int64_t realtime_end = 0;
	int64_t monotonic_end = 0;
	const struct timespec span = { 0, RATE_SPAN_MS * 1000000L };
	const bool read = wait_realtime_step(&realtime_start, &monotonic_start) && nanosleep(&span, NULL) == 0 &&
		wait_realtime_step(&realtime_end, &monotonic_end);
	const int64_t realtime_span = realtime_end - realtime_start;
	const int64_t monotonic_span = monotonic_end - monotonic_start;
	const int64_t error_ppm =
		read && realtime_span > 0 ? (monotonic_span - realtime_span) * 1000000 / realtime_span : 0;
	xsdk_log_write("clocks: over %lld ns of the kernel's system time CLOCK_MONOTONIC counted %lld ns, %lld ppm off",
		(long long)realtime_span, (long long)monotonic_span, (long long)error_ppm);
	check("clocks: CLOCK_MONOTONIC keeps the kernel's time to 0.05%",
		read && realtime_span > 0 && error_ppm > -RATE_TOLERANCE_PPM && error_ppm < RATE_TOLERANCE_PPM);
}

static void check_sleeps(void)
{
	const struct timespec request = { 0, SLEEP_NS };
	int64_t shortest = INT64_MAX;
	int64_t longest = 0;
	for (int i = 0; i < SLEEP_COUNT; i++) {
		struct timespec start;
		struct timespec end;
		(void)clock_gettime(CLOCK_MONOTONIC, &start);
		(void)nanosleep(&request, NULL);
		(void)clock_gettime(CLOCK_MONOTONIC, &end);
		const int64_t slept = nanoseconds(&end) - nanoseconds(&start);
		shortest = slept < shortest ? slept : shortest;
		longest = slept > longest ? slept : longest;
	}
	xsdk_log_write("clocks: %d sleeps of %d ns took %lld to %lld ns", SLEEP_COUNT, SLEEP_NS, (long long)shortest,
		(long long)longest);
	check("clocks: nanosleep never returns early", shortest >= SLEEP_NS);
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

	check_rate();
	check_sleeps();

	// Its 64 bits come back whole in r3; it reports 50 MHz, the time base's nominal rate, in Xenia and on the console
	// (results/2026-10-10-probe-vblank/xsdk.log).
	const uint64_t reported = KeQueryPerformanceFrequency();
	xsdk_log_write("clocks: KeQueryPerformanceFrequency reports %llu Hz", (unsigned long long)reported);
	check("clocks: KeQueryPerformanceFrequency, declared with its 64-bit return, reads 50 MHz", reported == 50000000);
}
