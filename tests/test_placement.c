#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Threads on the six hardware threads (SCOPE.md, milestone 26): where a thread starts and moves to, and how much work
// two busy threads get done on two cores and on one hardware thread.

enum {
	THREAD_STACK_BYTES = 64 * 1024,
	BUSY_MS = 300,
	// On the console two busy threads on two cores each got 93% to 100% of one's work done, and on one hardware thread
	// about half (results/2026-10-13-threads-probe/).
	APART_PERCENT_AT_LEAST = 80,
	SHARED_PERCENT_AT_MOST = 60,
	UNPLACED = -1,
	MOVE = -2,
};

// Five seconds, relative, in 100 ns units.
static const int64_t WAIT_TIMEOUT = -50000000;

typedef struct {
	// A worker records where it runs; one told to move goes to each hardware thread in turn and records each.
	int32_t task;
	uint32_t ran_on;
	uint32_t moved_to[XSDK_HARDWARE_THREADS];
	bool busy;
	uint64_t passes;
} Worker;

static volatile bool go;
static volatile uint32_t ready;

static uint64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

// Counts loop passes for BUSY_MS once every worker is ready; waits sleep, so workers sharing a hardware thread don't
// starve each other before the count.
static uint64_t busy_passes(void)
{
	__atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
	while (!go)
		(void)xsdk_thread_sleep(1);
	const uint64_t end = now_ns() + (uint64_t)BUSY_MS * 1000000u;
	volatile uint32_t sink = 0;
	uint64_t passes = 0;
	do {
		for (uint32_t i = 0; i < 4096; i++)
			sink += i;
		passes += 4096;
	} while (now_ns() < end);
	return passes;
}

static uint32_t work(void *context)
{
	Worker *worker = context;
	worker->ran_on = xsdk_thread_hardware_thread();
	if (worker->task == MOVE) {
		for (uint32_t i = 0; i < XSDK_HARDWARE_THREADS; i++)
			worker->moved_to[i] = NT_SUCCESS(xsdk_thread_move(i)) ? xsdk_thread_hardware_thread() : UINT32_MAX;
	}
	if (worker->busy)
		worker->passes = busy_passes();
	return 0;
}

// Starts each worker on its hardware thread, or where its creator runs, and waits for them all.
static bool run(Worker *workers, uint32_t count)
{
	HANDLE handles[XSDK_HARDWARE_THREADS];
	go = false;
	ready = 0;
	uint32_t started = 0;
	bool ok = true;
	while (ok && started < count) {
		Worker *worker = &workers[started];
		const NTSTATUS status = worker->task >= 0
			? xsdk_thread_create_on(&handles[started], THREAD_STACK_BYTES, work, worker, (uint32_t)worker->task)
			: xsdk_thread_create(&handles[started], THREAD_STACK_BYTES, work, worker);
		ok = NT_SUCCESS(status);
		started += ok;
	}
	uint32_t busy = 0;
	for (uint32_t i = 0; i < started; i++)
		busy += workers[i].busy;
	if (ok)
		while (ready < busy)
			(void)xsdk_thread_sleep(1);
	go = true;
	for (uint32_t i = 0; i < started; i++) {
		ok = ok && NtWaitForSingleObjectEx(handles[i], USER_MODE, 0, &WAIT_TIMEOUT) == STATUS_SUCCESS;
		(void)NtClose(handles[i]);
	}
	return ok;
}

static uint32_t percent(uint64_t passes, uint64_t alone)
{
	return alone ? (uint32_t)(passes * 100 / alone) : 0;
}

void test_placement(void)
{
	const uint32_t main_on = xsdk_thread_hardware_thread();
	xsdk_log_write("placement: the main thread runs on hardware thread %u", (unsigned)main_on);
	check("placement: the main thread runs on one of the six hardware threads", main_on < XSDK_HARDWARE_THREADS);

	Worker unplaced = { .task = UNPLACED };
	const bool unplaced_ran = run(&unplaced, 1);
	xsdk_log_write("placement: an unplaced thread ran on %u", (unsigned)unplaced.ran_on);
	check("placement: an unplaced thread runs where its creator does", unplaced_ran && unplaced.ran_on == main_on);

	Worker placed[XSDK_HARDWARE_THREADS];
	bool each_there = true;
	for (uint32_t i = 0; i < XSDK_HARDWARE_THREADS; i++) {
		placed[i] = (Worker){ .task = (int32_t)i };
		each_there = run(&placed[i], 1) && placed[i].ran_on == i && each_there;
	}
	xsdk_log_write("placement: threads started on 0 to 5 ran on %u %u %u %u %u %u", (unsigned)placed[0].ran_on,
		(unsigned)placed[1].ran_on, (unsigned)placed[2].ran_on, (unsigned)placed[3].ran_on, (unsigned)placed[4].ran_on,
		(unsigned)placed[5].ran_on);
	check("placement: a thread started on each hardware thread runs there", each_there);

	Worker mover = { .task = MOVE };
	bool moved = run(&mover, 1);
	for (uint32_t i = 0; i < XSDK_HARDWARE_THREADS; i++)
		moved = moved && mover.moved_to[i] == i;
	xsdk_log_write("placement: a thread moved to 0 to 5 ran on %u %u %u %u %u %u", (unsigned)mover.moved_to[0],
		(unsigned)mover.moved_to[1], (unsigned)mover.moved_to[2], (unsigned)mover.moved_to[3],
		(unsigned)mover.moved_to[4], (unsigned)mover.moved_to[5]);
	check("placement: a thread moved to each hardware thread runs there", moved);

	// One busy thread alone, then two on two cores, then two on one hardware thread: the first core's first hardware
	// thread and the second core's.
	Worker alone = { .task = 0, .busy = true };
	Worker apart[2] = { { .task = 0, .busy = true }, { .task = 2, .busy = true } };
	Worker shared[2] = { { .task = 0, .busy = true }, { .task = 0, .busy = true } };
	const bool busy_ran = run(&alone, 1) && run(apart, 2) && run(shared, 2);
	const uint32_t apart_least =
		percent(apart[0].passes < apart[1].passes ? apart[0].passes : apart[1].passes, alone.passes);
	const uint32_t shared_most =
		percent(shared[0].passes > shared[1].passes ? shared[0].passes : shared[1].passes, alone.passes);
	xsdk_log_write(
		"placement: one busy thread did %u passes in %u ms; two on two cores %u%% and %u%% of that, two on "
		"one hardware thread %u%% and %u%%",
		(unsigned)alone.passes, (unsigned)BUSY_MS, (unsigned)percent(apart[0].passes, alone.passes),
		(unsigned)percent(apart[1].passes, alone.passes), (unsigned)percent(shared[0].passes, alone.passes),
		(unsigned)percent(shared[1].passes, alone.passes));
	check("placement: two busy threads on two cores each get most of one's work done",
		busy_ran && apart_least >= APART_PERCENT_AT_LEAST);
	check("placement: two busy threads on one hardware thread share its time",
		busy_ran && shared_most <= SHARED_PERCENT_AT_MOST);
}
