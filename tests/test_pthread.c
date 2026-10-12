#include "tests.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/log.h"

enum {
	// Away from the kernel's default, so a thread given it can't pass with the default stack.
	ASKED_STACK_BYTES = 384 * 1024,
	// The kernel may round a stack up to its own unit.
	STACK_SLACK_BYTES = 64 * 1024,
	// Where the processor control region keeps the stack's high and low ends (xenia:src/xenia/kernel/xthread.h:78-79).
	PCR_STACK_BASE = 0x70,
	PCR_STACK_END = 0x74,
	DETACHED_WAIT_MS = 5000,
};

static uint32_t detached_ran;

static uint32_t current_stack_bytes(void)
{
	const uint8_t *pcr;
	__asm__("mr %0, 13" : "=r"(pcr));
	const uint32_t base = *(const volatile uint32_t *)(pcr + PCR_STACK_BASE);
	const uint32_t end = *(const volatile uint32_t *)(pcr + PCR_STACK_END);
	return base - end;
}

static void *report_stack(void *argument)
{
	(void)argument;
	return (void *)(uintptr_t)current_stack_bytes();
}

static void *mark_detached(void *argument)
{
	(void)argument;
	__atomic_store_n(&detached_ran, 1, __ATOMIC_RELEASE);
	return NULL;
}

// The stack a thread started with these attributes has, or 0 if it didn't start.
static uint32_t started_stack_bytes(const pthread_attr_t *attr)
{
	pthread_t thread;
	if (pthread_create(&thread, attr, report_stack, NULL) != 0)
		return 0;
	void *result = NULL;
	if (pthread_join(thread, &result) != 0)
		return 0;
	return (uint32_t)(uintptr_t)result;
}

static bool detached_thread_runs(void)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	const int created = pthread_create(&thread, &attr, mark_detached, NULL);
	pthread_attr_destroy(&attr);
	if (created != 0)
		return false;
	const struct timespec millisecond = { .tv_nsec = 1000000 };
	for (int waited_ms = 0; waited_ms < DETACHED_WAIT_MS; waited_ms++) {
		if (__atomic_load_n(&detached_ran, __ATOMIC_ACQUIRE))
			return true;
		(void)nanosleep(&millisecond, NULL);
	}
	return false;
}

static bool bad_attributes_refused(void)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	const bool refused =
		pthread_attr_setdetachstate(&attr, -1) == EINVAL && pthread_attr_setstacksize(&attr, 1) == EINVAL;
	pthread_attr_destroy(&attr);
	return refused;
}

static bool one_priority_level(void)
{
	int policy = -1;
	struct sched_param param = { .sched_priority = -1 };
	const bool got = pthread_getschedparam(pthread_self(), &policy, &param) == 0;
	const struct sched_param higher = { .sched_priority = 1 };
	return got && policy == SCHED_OTHER && param.sched_priority == 0 && sched_get_priority_min(SCHED_OTHER) == 0 &&
		sched_get_priority_max(SCHED_OTHER) == 0 &&
		pthread_setschedparam(pthread_self(), SCHED_OTHER, &higher) == ENOTSUP;
}

void test_pthread(void)
{
	const uint32_t default_bytes = started_stack_bytes(NULL);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, ASKED_STACK_BYTES);
	const uint32_t asked_bytes = started_stack_bytes(&attr);
	pthread_attr_destroy(&attr);
	xsdk_log_write("pthread: a thread's stack is %u bytes by default, %u when %u are asked for",
		(unsigned)default_bytes, (unsigned)asked_bytes, (unsigned)ASKED_STACK_BYTES);
	check("pthread: a thread gets the stack size its attributes ask for",
		asked_bytes >= ASKED_STACK_BYTES && asked_bytes < ASKED_STACK_BYTES + STACK_SLACK_BYTES);
	check("pthread: a thread made detached runs", detached_thread_runs());
	check("pthread: a bad detach state and a one-byte stack are refused", bad_attributes_refused());
	check("pthread: priorities report one level and refuse a change", one_priority_level());
}
