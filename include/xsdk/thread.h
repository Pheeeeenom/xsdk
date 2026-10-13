#ifndef XSDK_THREAD_H
#define XSDK_THREAD_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*XsdkThreadEntry)(void *context);

// The console's hardware threads, two to a core: 0 and 1, 2 and 3, 4 and 5 (SCOPE.md, milestone 26).
enum { XSDK_HARDWARE_THREADS = 6 };

// Starts entry(context) on a new thread; its return value becomes the exit code. 0 stack bytes means the default. The
// thread runs on its creator's hardware thread, as the kernel places it.
NTSTATUS xsdk_thread_create(HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context);
// As xsdk_thread_create, on the given hardware thread.
NTSTATUS xsdk_thread_create_on(
	HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context, uint32_t hardware_thread);
// Moves the calling thread to the given hardware thread.
NTSTATUS xsdk_thread_move(uint32_t hardware_thread);
// The hardware thread the calling thread runs on.
uint32_t xsdk_thread_hardware_thread(void);
NTSTATUS xsdk_thread_sleep(uint32_t milliseconds);
// Totals across all threads, to show that a wait sleeps rather than spins.
void xsdk_thread_wait_counts(uint32_t *yields, uint32_t *blocking_waits);

#ifdef __cplusplus
}
#endif

#endif
