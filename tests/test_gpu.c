#include <stdint.h>

#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	FENCE_TIMEOUT_MS = 1000,
	// Each fence gives the ring a segment of 14 dwords, so the 8192-dword ring wraps a few times.
	WRAP_ROUNDS = 4000,
	// 25600 dwords with no fence, which the ring runs in segments of 1024 or a little more.
	LONG_WRITE_NOPS = 400,
	NOP_DWORDS = 64,
	SEGMENT_DWORDS = 1024,
	// 5 MB, so the 4 MB stream wraps, with a fence every 64 KB.
	STREAM_WRAP_NOPS = 20480,
	NOPS_A_FENCE = 256,
};

static void put_nop(uint32_t payload_dwords)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + payload_dwords);
	packet[0] = xsdk_gpu_type3(PM4_NOP, payload_dwords);
	for (uint32_t i = 1; i <= payload_dwords; i++)
		packet[i] = 0;
}

// A NOP of 4 dwords, then a fence that kicks once; then a long write with no fence, which still runs.
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

	for (uint32_t i = 0; i < LONG_WRITE_NOPS; i++)
		put_nop(NOP_DWORDS - 1);
	xsdk_gpu_stats(&after);
	const uint32_t kicks = after.kicks - before.kicks;
	const bool ran = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	xsdk_log_write(
		"gpu: %u dwords with no fence: %u segments", (unsigned)(after.dwords - before.dwords), (unsigned)kicks);
	check("gpu: a long write runs in segments, before its fence",
		after.dwords - before.dwords >= LONG_WRITE_NOPS * NOP_DWORDS &&
			kicks >= LONG_WRITE_NOPS * NOP_DWORDS / (2 * SEGMENT_DWORDS) && ran);
}

// More than the stream holds, so it wraps and reuses its blocks behind the GPU.
static void test_stream_wrap(void)
{
	XsdkGpuStats before;
	XsdkGpuStats after;
	xsdk_gpu_stats(&before);
	uint32_t fence = 0;
	for (uint32_t i = 1; i <= STREAM_WRAP_NOPS; i++) {
		put_nop(NOP_DWORDS - 1);
		if (i % NOPS_A_FENCE == 0)
			fence = xsdk_gpu_fence();
	}
	const bool ran = NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS));
	xsdk_gpu_stats(&after);
	xsdk_log_write("gpu: %u dwords as the stream wraps: %u segments, %u waits, %u us waiting",
		(unsigned)(after.dwords - before.dwords), (unsigned)(after.kicks - before.kicks),
		(unsigned)(after.waits - before.waits), (unsigned)(after.waited_us - before.waited_us));
	check("gpu: fences as the stream wraps", ran && after.dwords - before.dwords >= STREAM_WRAP_NOPS * NOP_DWORDS);
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
	test_stream_wrap();

	// NOPs of different sizes, so the ring wraps at different offsets.
	uint32_t fence = 0;
	for (uint32_t i = 0; i < WRAP_ROUNDS; i++) {
		put_nop(1 + i % 4);
		fence = xsdk_gpu_fence();
	}
	check("gpu: fences as the ring wraps", NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS)));
	check("gpu: close", NT_SUCCESS(xsdk_gpu_close()));
}
