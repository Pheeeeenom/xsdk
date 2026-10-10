#ifndef XSDK_THREAD_H
#define XSDK_THREAD_H

#include <stdint.h>

#include "xsdk/kernel.h"

typedef uint32_t (*XsdkThreadEntry)(void *context);

// Starts entry(context) on a new thread; its return value becomes the exit code. 0 stack bytes means the default.
NTSTATUS xsdk_thread_create(HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context);
NTSTATUS xsdk_thread_sleep(uint32_t milliseconds);
// Totals across all threads, to show that a wait sleeps rather than spins.
void xsdk_thread_wait_counts(uint32_t *yields, uint32_t *blocking_waits);

#endif
