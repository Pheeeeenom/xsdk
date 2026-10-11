#ifndef XSDK_GPU_H
#define XSDK_GPU_H

#include <stdbool.h>
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
// Waits until the GPU has passed the fence, or returns STATUS_IO_TIMEOUT.
NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms);
// Whether the GPU has passed the fence, without waiting; safe in the graphics interrupt callback.
bool xsdk_gpu_fence_done(uint32_t fence);

// Write-combined physical memory for the GPU, in 64 KB pages, or NULL. Write it with volatile 32-bit stores.
volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes);
void xsdk_gpu_free(volatile uint32_t *memory);
// The address the GPU uses for memory from xsdk_gpu_alloc.
uint32_t xsdk_gpu_address(volatile const uint32_t *memory);
// Queues copying the eDRAM colour target to address as tiled k_8_8_8_8, then clearing it; width a multiple of 32.
void xsdk_gpu_resolve(uint32_t address, uint32_t width, uint32_t height, uint32_t clear_argb);
// The bytes such a resolve writes, in whole 32-row tiles (xenia:src/xenia/gpu/draw_util.cc:1055-1062).
uint32_t xsdk_gpu_resolve_bytes(uint32_t width, uint32_t height);
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

// Shader constants for the draws after, written from a copy in whole blocks: partial writes hung the GPU
// (docs/HARDWARE.md).
enum {
	// Four floats each; each stage reads its own 256.
	XSDK_FLOAT_CONSTANTS = 256,
	// 32 to a word; libxenon gives vertex shaders the first 128 and pixel shaders the rest (xe.c:2038-2064).
	XSDK_BOOL_CONSTANT_WORDS = 8,
	XSDK_LOOP_CONSTANTS = 32,
};

// Sets the vertex shader's float constants first to first + count - 1 from count * 4 floats, rewriting each 16 touched.
void xsdk_gpu_set_vertex_constants(uint32_t first, const float *values, uint32_t count);
// The same for the pixel shader's float constants.
void xsdk_gpu_set_pixel_constants(uint32_t first, const float *values, uint32_t count);
// Sets words of boolean constants, bit n of word w being constant 32 * w + n, as a shader's cexec names it.
void xsdk_gpu_set_bool_constants(uint32_t first_word, const uint32_t *words, uint32_t count);
// Sets loop constants from xsdk_gpu_loop_constant. These and the booleans are always rewritten all together.
void xsdk_gpu_set_loop_constants(uint32_t first, const uint32_t *values, uint32_t count);

// A loop of count passes, aL running from start by step (xenia:src/xenia/gpu/xenos.h:1086-1097).
static inline uint32_t xsdk_gpu_loop_constant(uint32_t count, uint32_t start, int32_t step)
{
	return (count & 0xFF) | (start & 0xFF) << 8 | ((uint32_t)step & 0xFF) << 16;
}

#ifdef __cplusplus
}
#endif

#endif
