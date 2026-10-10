#ifndef XSDK_GPU_H
#define XSDK_GPU_H

#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

#ifdef __cplusplus
extern "C" {
#endif

// The command processor's ring: PM4 packets the title writes and the GPU reads. One thread at a time.

// Type-3 packet opcodes (xenia:src/xenia/gpu/xenos.h:1572-1622).
enum {
	PM4_NOP = 0x10,
};

// Primitive types (xenia:src/xenia/gpu/xenos.h:44-49).
enum {
	XSDK_PRIMITIVE_TRIANGLE_LIST = 4,
};

// The header of a type-3 packet with payload_dwords dwords after it (xenia:src/xenia/gpu/xenos.h:1657-1664).
static inline uint32_t xsdk_gpu_type3(uint32_t opcode, uint32_t payload_dwords)
{
	return 3u << 30 | (payload_dwords - 1) << 16 | opcode << 8;
}

// Takes the GPU from XAM's D3D device, sets up the ring, and sets the GPU up as libxenon does (docs/HARDWARE.md).
NTSTATUS xsdk_gpu_open(void);
// Writes back the registers the SDK changed and gives the GPU back to XAM. The title's exit calls it too.
NTSTATUS xsdk_gpu_close(void);
// Returns room for dword_count dwords; write them all, with volatile stores, before the next call.
volatile uint32_t *xsdk_gpu_reserve(uint32_t dword_count);
// Submits everything written so far, then a fence; returns the fence's number.
uint32_t xsdk_gpu_fence(void);
// Waits until the GPU has passed the fence, or returns STATUS_TIMEOUT.
NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms);

// Write-combined physical memory for the GPU, in 64 KB pages, or NULL. Write it with volatile 32-bit stores.
volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes);
void xsdk_gpu_free(volatile uint32_t *memory);
// The address the GPU uses for memory from xsdk_gpu_alloc.
uint32_t xsdk_gpu_address(volatile const uint32_t *memory);
// Queues copying the eDRAM colour target to address as tiled k_8_8_8_8, then clearing it; width a multiple of 32.
void xsdk_gpu_resolve(uint32_t address, uint32_t width, uint32_t height, uint32_t clear_argb);
// Queues the same into a tiled texture, drawn at its size, leaving ARGB words as xsdk_texture_upload does.
void xsdk_gpu_resolve_texture(const XsdkTexture *texture, uint32_t clear_argb);

// Sets the eDRAM colour target and a viewport, no depth, blending or culling (libxenon); a resolve undoes it.
void xsdk_gpu_set_target(uint32_t width, uint32_t height);
// Loads both shaders with IM_LOAD and sets the program registers; a resolve replaces the vertex shader.
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel);
// Points vertex stream 0 at vertices from xsdk_gpu_alloc, clearing fetch constants 93 and 94, which share its group.
void xsdk_gpu_set_vertices(uint32_t address, uint32_t size_bytes);
// Points texture fetch constant slot, 0 to 31, at the texture; a resolve overwrites slot 0, xsdk_gpu_set_vertices 31.
void xsdk_gpu_set_texture(uint32_t slot, const XsdkTexture *texture);
// Draws vertex_count auto-indexed vertices as a list of primitive_type.
void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count);

#ifdef __cplusplus
}
#endif

#endif
