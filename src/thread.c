#include "xsdk/thread.h"

#include "pthread_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

void xsdk_thread_entry(XsdkThreadEntry entry, void *context);
void xsdk_thread_run(XsdkThreadEntry entry, void *context);

void xsdk_thread_run(XsdkThreadEntry entry, void *context)
{
	const uint32_t exit_code = entry(context);
	xsdk_pthread_thread_end();
	ExTerminateThread(exit_code);
}

NTSTATUS xsdk_thread_create(HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context)
{
	uint32_t thread_id;
	NTSTATUS status =
		ExCreateThread(thread, stack_bytes, &thread_id, (void *)xsdk_thread_entry, (void *)entry, context, 0);
	if (!NT_SUCCESS(status))
		xsdk_log_write("ExCreateThread failed: 0x%08X", (unsigned)status);
	return status;
}

NTSTATUS xsdk_thread_sleep(uint32_t milliseconds)
{
	// A negative interval is relative, in 100 ns units.
	const int64_t interval = -(int64_t)milliseconds * 10000;
	return KeDelayExecutionThread(USER_MODE, 0, &interval);
}
