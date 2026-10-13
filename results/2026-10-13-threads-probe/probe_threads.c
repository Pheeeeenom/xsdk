#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Which of the six hardware threads a thread runs on, and what placing it changes: where a new thread starts, whether
// KeSetAffinityThread and ExCreateThread's processor bits move it, and how much work busy threads get done sharing a
// hardware thread, on one core's two hardware threads, and on separate cores. Dusklight's title scene runs at 20 fps on
// the console and 30 in Xenia, which places unplaced threads round-robin (xenia:src/xenia/kernel/xthread.cc:159-169).

enum {
	SPLASH_MS = 2000,
	SPIN_MS = 500,
	HARDWARE_THREADS = 6,
	// ExCreateThread takes a processor mask in its flags' top byte (xenia:src/xenia/kernel/xthread.cc:355-356).
	CREATE_PROCESSOR_SHIFT = 24,
	UNPLACED = -1,
};

// 1 places workers with KeSetAffinityThread once they run; 0 creates them placed, with ExCreateThread's processor
// bits, and leaves KeSetAffinityThread unimported (the console wouldn't launch the build that imports it).
#ifndef PROBE_PLACE_BY_AFFINITY
#define PROBE_PLACE_BY_AFFINITY 1
#endif

// The kernel calls it with the entry and context ExCreateThread was given (src/thread.c).
void xsdk_thread_run(XsdkThreadEntry entry, void *context);

typedef struct {
	int32_t place;
	uint32_t started_on;
	uint32_t ran_on;
	uint32_t ended_on;
	NTSTATUS place_status;
	uint32_t previous_affinity;
	uint64_t iterations;
} Worker;

static volatile bool go;
static volatile uint32_t ready;
static volatile uint32_t done;

static uint64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

// Places itself if asked, then counts loop passes for SPIN_MS once every worker is ready. Waits sleep, so workers on
// one hardware thread don't starve each other before the count.
static uint32_t worker_run(void *context)
{
	Worker *worker = context;
	worker->started_on = xsdk_current_hardware_thread();
#if PROBE_PLACE_BY_AFFINITY
	if (worker->place != UNPLACED)
		worker->place_status = KeSetAffinityThread(
			xsdk_current_kthread(), 1u << worker->place, &worker->previous_affinity);
#endif
	worker->ran_on = xsdk_current_hardware_thread();
	__atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
	while (!go)
		(void)xsdk_thread_sleep(1);
	const uint64_t end = now_ns() + (uint64_t)SPIN_MS * 1000000u;
	volatile uint32_t sink = 0;
	uint64_t passes = 0;
	do {
		for (uint32_t i = 0; i < 4096; i++)
			sink += i;
		passes += 4096;
	} while (now_ns() < end);
	worker->iterations = passes;
	worker->ended_on = xsdk_current_hardware_thread();
	__atomic_add_fetch(&done, 1, __ATOMIC_SEQ_CST);
	return 0;
}

// Runs the workers at once, each placed as asked, created with ExCreateThread's processor bits when create_on is a
// hardware thread; logs where each ran and its passes against the baseline's.
static uint64_t run_case(const char *name, const int32_t *places, uint32_t count, int32_t create_on, uint64_t baseline)
{
	Worker workers[HARDWARE_THREADS] = { 0 };
	HANDLE handles[HARDWARE_THREADS] = { 0 };
	go = false;
	ready = 0;
	done = 0;
	for (uint32_t i = 0; i < count; i++) {
		workers[i].place = places[i];
		NTSTATUS status;
		const int32_t processor = PROBE_PLACE_BY_AFFINITY || places[i] == UNPLACED ? create_on : places[i];
		if (processor != UNPLACED) {
			uint32_t thread_id;
			status = ExCreateThread(&handles[i], 0, &thread_id, (void *)xsdk_thread_run, (void *)worker_run,
				&workers[i], (1u << processor) << CREATE_PROCESSOR_SHIFT);
		} else {
			status = xsdk_thread_create(&handles[i], 0, worker_run, &workers[i]);
		}
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("threads: %s: creating worker %u failed: 0x%08X", name, (unsigned)i, (unsigned)status);
			return 0;
		}
	}
	while (ready < count)
		(void)xsdk_thread_sleep(1);
	go = true;
	while (done < count)
		(void)xsdk_thread_sleep(10);
	xsdk_log_write("threads: %s", name);
	for (uint32_t i = 0; i < count; i++) {
		const Worker *worker = &workers[i];
		xsdk_log_write("threads:   worker %u: started on %u, ran on %u, ended on %u; placed %d (0x%08X, was 0x%02X); "
					   "%u passes, %u%% of one alone",
			(unsigned)i, (unsigned)worker->started_on, (unsigned)worker->ran_on, (unsigned)worker->ended_on,
			(int)worker->place, (unsigned)worker->place_status, (unsigned)worker->previous_affinity,
			(unsigned)worker->iterations,
			baseline ? (unsigned)(worker->iterations * 100 / baseline) : 100u);
		(void)NtClose(handles[i]);
	}
	return workers[0].iterations;
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	const uint32_t main_on = xsdk_current_hardware_thread();
	// Hardware threads come in pairs, one core's two (xenia:src/xenia/kernel/xthread.cc:160).
	const int32_t sibling = (int32_t)(main_on ^ 1);
	const int32_t other_core = (int32_t)((main_on + 2) % HARDWARE_THREADS);
	const int32_t third_core = (int32_t)((main_on + 4) % HARDWARE_THREADS);
	xsdk_log_write("threads: main on hardware thread %u, its KTHREAD at 0x%08X", (unsigned)main_on,
		(unsigned)(uintptr_t)xsdk_current_kthread());

	const int32_t unplaced[HARDWARE_THREADS] = { UNPLACED, UNPLACED, UNPLACED, UNPLACED, UNPLACED, UNPLACED };
	const uint64_t alone = run_case("one worker, unplaced", unplaced, 1, UNPLACED, 0);
	run_case("two workers, unplaced", unplaced, 2, UNPLACED, alone);
	run_case("six workers, unplaced", unplaced, 6, UNPLACED, alone);
	const int32_t same[2] = { (int32_t)main_on, (int32_t)main_on };
	run_case("two workers, both placed on main's hardware thread", same, 2, UNPLACED, alone);
	const int32_t pair[2] = { (int32_t)main_on, sibling };
	run_case("two workers, placed on one core's two hardware threads", pair, 2, UNPLACED, alone);
	const int32_t apart[2] = { (int32_t)main_on, other_core };
	run_case("two workers, placed on two cores", apart, 2, UNPLACED, alone);
	const int32_t each[HARDWARE_THREADS] = { 0, 1, 2, 3, 4, 5 };
	run_case("six workers, one placed on each hardware thread", each, 6, UNPLACED, alone);
	run_case("one worker, created with ExCreateThread's processor bits for the third core", unplaced, 1, third_core,
		alone);
	xsdk_log_write("threads: done");
	return 0;
}
