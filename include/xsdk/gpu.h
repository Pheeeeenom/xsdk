#ifndef XSDK_GPU_H
#define XSDK_GPU_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// The command processor's ring: PM4 packets the title writes and the GPU reads. One thread at a time.

// Type-3 packet opcodes (xenia:src/xenia/gpu/xenos.h:1572-1622).
enum {
	PM4_NOP = 0x10,
};

// The header of a type-3 packet with payload_dwords dwords after it (xenia:src/xenia/gpu/xenos.h:1657-1664).
static inline uint32_t xsdk_gpu_type3(uint32_t opcode, uint32_t payload_dwords)
{
	return 3u << 30 | (payload_dwords - 1) << 16 | opcode << 8;
}

// Takes the GPU from XAM's D3D device and sets up the ring (docs/HARDWARE.md).
NTSTATUS xsdk_gpu_open(void);
// Waits for the GPU to finish and gives it back to XAM. The title's exit calls it too.
NTSTATUS xsdk_gpu_close(void);
// Returns room for dword_count dwords; write them all, with volatile stores, before the next call.
volatile uint32_t *xsdk_gpu_reserve(uint32_t dword_count);
// Submits everything written so far, then a fence; returns the fence's number.
uint32_t xsdk_gpu_fence(void);
// Waits until the GPU has passed the fence, or returns STATUS_TIMEOUT.
NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
