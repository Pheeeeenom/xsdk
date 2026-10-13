#include "xsdk/thread.h"

#include "pthread_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	// ExCreateThread takes a hardware thread mask in its flags' top byte (xenia:src/xenia/kernel/xthread.cc:355-356).
	CREATE_HARDWARE_THREAD_SHIFT = 24,
	// The processor control region's thread object and hardware thread (xenia:src/xenia/kernel/xthread.h:81-83).
	PCR_CURRENT_THREAD = 0x100,
	PCR_CURRENT_PROCESSOR = 0x10C,
};

void xsdk_thread_run(XsdkThreadEntry entry, void *context);

// The kernel calls it with the thread's start address and context (xenia:src/xenia/kernel/xthread.cc:518-521).
void xsdk_thread_run(XsdkThreadEntry entry, void *context)
{
	const uint32_t exit_code = entry(context);
	xsdk_pthread_thread_end();
	ExTerminateThread(exit_code);
}

static NTSTATUS create(HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context, uint32_t flags)
{
	uint32_t thread_id;
	NTSTATUS status =
		ExCreateThread(thread, stack_bytes, &thread_id, (void *)xsdk_thread_run, (void *)entry, context, flags);
	if (!NT_SUCCESS(status))
		xsdk_log_write("ExCreateThread failed: 0x%08X", (unsigned)status);
	return status;
}

NTSTATUS xsdk_thread_create(HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context)
{
	return create(thread, stack_bytes, entry, context, 0);
}

NTSTATUS xsdk_thread_create_on(
	HANDLE *thread, uint32_t stack_bytes, XsdkThreadEntry entry, void *context, uint32_t hardware_thread)
{
	if (hardware_thread >= XSDK_HARDWARE_THREADS)
		return STATUS_INVALID_PARAMETER;
	return create(thread, stack_bytes, entry, context, (1u << hardware_thread) << CREATE_HARDWARE_THREAD_SHIFT);
}

// The calling thread's processor control region, which r13 points at on every thread; 0x801B0000 on hardware thread 0
// (docs/HARDWARE.md, "Calls between xsdk's GCC and the console's MSVC"). GCC never allocates r13 on xsdk's target,
// which takes System V's fixed small data register (gcc:gcc/config/rs6000/sysv4.h:265-267).
static volatile uint8_t *processor_control_region(void)
{
	uint32_t region;
	__asm__ volatile("mr %0, 13" : "=r"(region));
	return (volatile uint8_t *)(uintptr_t)region;
}

NTSTATUS xsdk_thread_move(uint32_t hardware_thread)
{
	if (hardware_thread >= XSDK_HARDWARE_THREADS)
		return STATUS_INVALID_PARAMETER;
	void *const self = (void *)(uintptr_t)*(volatile uint32_t *)(processor_control_region() + PCR_CURRENT_THREAD);
	uint32_t previous;
	return KeSetAffinityThread(self, 1u << hardware_thread, &previous);
}

uint32_t xsdk_thread_hardware_thread(void)
{
	return processor_control_region()[PCR_CURRENT_PROCESSOR];
}

NTSTATUS xsdk_thread_sleep(uint32_t milliseconds)
{
	// A negative interval is relative, in 100 ns units.
	const int64_t interval = -(int64_t)milliseconds * 10000;
	return KeDelayExecutionThread(USER_MODE, 0, &interval);
}
