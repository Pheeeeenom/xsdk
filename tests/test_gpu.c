#include <stdint.h>

#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	FENCE_TIMEOUT_MS = 1000,
	// About 30000 dwords, so the 8192-dword ring wraps a few times.
	WRAP_ROUNDS = 4000,
	// 25600 dwords with no fence, so the ring fills about three times.
	FULL_RING_NOPS = 400,
	FULL_RING_NOP_DWORDS = 64,
};

static void put_nop(uint32_t payload_dwords)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + payload_dwords);
	packet[0] = xsdk_gpu_type3(PM4_NOP, payload_dwords);
	for (uint32_t i = 1; i <= payload_dwords; i++)
		packet[i] = 0;
}

// A NOP of 4 dwords, then a fence that kicks once; then more NOPs than the ring holds, so it waits for room.
static void test_stats(void)
{
	XsdkGpuStats before;
	XsdkGpuStats after;
	xsdk_gpu_stats(&before);
	put_nop(3);
	xsdk_gpu_stats(&after);
	const bool counted_nop = after.dwords - before.dwords == 4 && after.kicks == before.kicks;
	(void)xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_gpu_stats(&before);
	check("gpu: stats count the dwords written and the fence's kick", counted_nop && before.kicks == after.kicks + 1);

	for (uint32_t i = 0; i < FULL_RING_NOPS; i++)
		put_nop(FULL_RING_NOP_DWORDS - 1);
	xsdk_gpu_stats(&after);
	const uint32_t waits = after.waits - before.waits;
	const uint32_t sleeps = after.sleeps - before.sleeps;
	const uint32_t waited_us = (uint32_t)(after.waited_us - before.waited_us);
	xsdk_log_write("gpu: %u dwords with no fence: %u waits for room, %u sleeps, %u us waiting",
		(unsigned)(after.dwords - before.dwords), (unsigned)waits, (unsigned)sleeps, (unsigned)waited_us);
	check("gpu: stats count a full ring's waits, sleeps and time",
		after.dwords - before.dwords >= FULL_RING_NOPS * FULL_RING_NOP_DWORDS && waits >= 2 && sleeps >= 1 &&
			waited_us > 0);
	(void)xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
}

void test_gpu(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status))
		xsdk_log_write("gpu: xsdk_gpu_open returned 0x%08X", (unsigned)status);
	check("gpu: open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;

	put_nop(1);
	check("gpu: NOP and fence", NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)));
	test_stats();

	// NOPs of different sizes, so the ring wraps at different offsets.
	uint32_t fence = 0;
	for (uint32_t i = 0; i < WRAP_ROUNDS; i++) {
		put_nop(1 + i % 4);
		fence = xsdk_gpu_fence();
	}
	check("gpu: fences as the ring wraps", NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS)));
	check("gpu: close", NT_SUCCESS(xsdk_gpu_close()));
}
