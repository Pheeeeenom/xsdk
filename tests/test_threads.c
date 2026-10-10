#include <stdint.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	ITEM_COUNT = 100,
	QUEUE_SLOTS = 8,
	THREAD_STACK_BYTES = 64 * 1024,
};

// Five seconds, relative, in 100 ns units.
static const int64_t WAIT_TIMEOUT = -50000000;

// A producer hands numbers to a consumer through a ring guarded by two semaphores; an event reports the sum.
typedef struct {
	HANDLE items;
	HANDLE space;
	HANDLE done;
	uint32_t queue[QUEUE_SLOTS];
	uint32_t sum;
} Pipe;

static uint32_t produce(void *context)
{
	Pipe *pipe = context;
	for (uint32_t i = 1; i <= ITEM_COUNT; i++) {
		(void)NtWaitForSingleObjectEx(pipe->space, USER_MODE, 0, NULL);
		pipe->queue[i % QUEUE_SLOTS] = i;
		(void)NtReleaseSemaphore(pipe->items, 1, NULL);
	}
	xsdk_log_write("threads: producer queued %d items", ITEM_COUNT);
	return 1;
}

static uint32_t consume(void *context)
{
	Pipe *pipe = context;
	for (uint32_t i = 1; i <= ITEM_COUNT; i++) {
		(void)NtWaitForSingleObjectEx(pipe->items, USER_MODE, 0, NULL);
		pipe->sum += pipe->queue[i % QUEUE_SLOTS];
		(void)NtReleaseSemaphore(pipe->space, 1, NULL);
	}
	xsdk_log_write("threads: consumer summed %d items", ITEM_COUNT);
	(void)NtSetEvent(pipe->done, NULL);
	return 2;
}

static bool create_objects(Pipe *pipe)
{
	NTSTATUS status = NtCreateSemaphore(&pipe->items, NULL, 0, QUEUE_SLOTS);
	if (NT_SUCCESS(status))
		status = NtCreateSemaphore(&pipe->space, NULL, QUEUE_SLOTS, QUEUE_SLOTS);
	if (NT_SUCCESS(status))
		status = NtCreateEvent(&pipe->done, NULL, SYNCHRONIZATION_EVENT, 0);
	if (!NT_SUCCESS(status))
		xsdk_log_write("threads: creating semaphores and event failed: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status);
}

void test_threads(void)
{
	static Pipe pipe;
	if (!create_objects(&pipe)) {
		check("threads hand work over", false);
		return;
	}
	HANDLE producer;
	HANDLE consumer;
	if (!NT_SUCCESS(xsdk_thread_create(&consumer, THREAD_STACK_BYTES, consume, &pipe)) ||
		!NT_SUCCESS(xsdk_thread_create(&producer, THREAD_STACK_BYTES, produce, &pipe))) {
		check("threads hand work over", false);
		return;
	}

	const NTSTATUS done = NtWaitForSingleObjectEx(pipe.done, USER_MODE, 0, &WAIT_TIMEOUT);
	xsdk_log_write("threads: done event 0x%08X, sum %u", (unsigned)done, (unsigned)pipe.sum);
	check("threads hand work over", done == STATUS_SUCCESS && pipe.sum == ITEM_COUNT * (ITEM_COUNT + 1) / 2);

	const NTSTATUS producer_exit = NtWaitForSingleObjectEx(producer, USER_MODE, 0, &WAIT_TIMEOUT);
	const NTSTATUS consumer_exit = NtWaitForSingleObjectEx(consumer, USER_MODE, 0, &WAIT_TIMEOUT);
	check("threads exit", producer_exit == STATUS_SUCCESS && consumer_exit == STATUS_SUCCESS);
	(void)NtClose(producer);
	(void)NtClose(consumer);
	(void)NtClose(pipe.items);
	(void)NtClose(pipe.space);
	(void)NtClose(pipe.done);
}
