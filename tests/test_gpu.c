#include <stdint.h>

#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	FENCE_TIMEOUT_MS = 1000,
	// About 30000 dwords, so the 8192-dword ring wraps a few times.
	WRAP_ROUNDS = 4000,
};

static void put_nop(uint32_t payload_dwords)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + payload_dwords);
	packet[0] = xsdk_gpu_type3(PM4_NOP, payload_dwords);
	for (uint32_t i = 1; i <= payload_dwords; i++)
		packet[i] = 0;
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

	// NOPs of different sizes, so the ring wraps at different offsets.
	uint32_t fence = 0;
	for (uint32_t i = 0; i < WRAP_ROUNDS; i++) {
		put_nop(1 + i % 4);
		fence = xsdk_gpu_fence();
	}
	check("gpu: fences as the ring wraps", NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS)));
	check("gpu: close", NT_SUCCESS(xsdk_gpu_close()));
}
