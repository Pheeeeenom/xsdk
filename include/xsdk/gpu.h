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

// Primitive types (xenia:src/xenia/gpu/xenos.h:44-51).
enum {
	XSDK_PRIMITIVE_POINT_LIST = 1,
	XSDK_PRIMITIVE_LINE_LIST = 2,
	XSDK_PRIMITIVE_LINE_STRIP = 3,
	XSDK_PRIMITIVE_TRIANGLE_LIST = 4,
	XSDK_PRIMITIVE_TRIANGLE_STRIP = 6,
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
// Points fetch constant slot 0 to 31 at the texture and sampler; a resolve takes slot 0, xsdk_gpu_set_vertices 31.
void xsdk_gpu_set_texture(uint32_t slot, const XsdkTexture *texture, const XsdkSamplerState *sampler);
// Draws vertex_count auto-indexed vertices as primitive_type.
void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count);
// Draws up to 65535 indices as primitive_type, big-endian, 16 or 32 bits each, from memory from xsdk_gpu_alloc.
void xsdk_gpu_draw_indexed(uint32_t primitive_type, uint32_t index_address, uint32_t index_count, bool indices_32_bit);

// Comparisons (xenia:src/xenia/gpu/xenos.h:685-694).
enum {
	XSDK_COMPARE_NEVER,
	XSDK_COMPARE_LESS,
	XSDK_COMPARE_EQUAL,
	XSDK_COMPARE_LESS_EQUAL,
	XSDK_COMPARE_GREATER,
	XSDK_COMPARE_NOT_EQUAL,
	XSDK_COMPARE_GREATER_EQUAL,
	XSDK_COMPARE_ALWAYS,
};

// Stencil operations (xenia:src/xenia/gpu/xenos.h:696-705).
enum {
	XSDK_STENCIL_KEEP,
	XSDK_STENCIL_ZERO,
	XSDK_STENCIL_REPLACE,
	XSDK_STENCIL_INCREMENT_CLAMP,
	XSDK_STENCIL_DECREMENT_CLAMP,
	XSDK_STENCIL_INVERT,
	XSDK_STENCIL_INCREMENT_WRAP,
	XSDK_STENCIL_DECREMENT_WRAP,
};

// How draws test and write the target's 24-bit depth and 8-bit stencil, the same for both faces.
typedef struct {
	bool depth_test;
	bool depth_write;
	uint32_t depth_compare;
	bool stencil_test;
	uint32_t stencil_compare;
	uint32_t stencil_fail;
	uint32_t stencil_depth_fail;
	uint32_t stencil_pass;
	uint8_t stencil_reference;
	uint8_t stencil_read_mask;
	uint8_t stencil_write_mask;
} XsdkDepthStencil;

// The registers a depth/stencil setting writes, worked out once.
typedef struct {
	uint32_t depth_control;
	uint32_t stencil_ref_mask;
} XsdkDepthStencilState;

void xsdk_gpu_depth_stencil_state(XsdkDepthStencilState *state, const XsdkDepthStencil *setting);
// Applies to the draws after; xsdk_gpu_set_target and resolves turn depth and stencil off.
void xsdk_gpu_set_depth_stencil(const XsdkDepthStencilState *state);
// The depth, 0 to 1, and stencil the later resolves clear to; far and 0 until set.
void xsdk_gpu_set_depth_clear(float depth, uint8_t stencil);

// Blend factors (xenia:src/xenia/gpu/xenos.h:708-723), less the constant-colour ones, and operations (727-733).
enum {
	XSDK_BLEND_ZERO = 0,
	XSDK_BLEND_ONE = 1,
	XSDK_BLEND_SRC_COLOR = 4,
	XSDK_BLEND_ONE_MINUS_SRC_COLOR = 5,
	XSDK_BLEND_SRC_ALPHA = 6,
	XSDK_BLEND_ONE_MINUS_SRC_ALPHA = 7,
	XSDK_BLEND_DST_COLOR = 8,
	XSDK_BLEND_ONE_MINUS_DST_COLOR = 9,
	XSDK_BLEND_DST_ALPHA = 10,
	XSDK_BLEND_ONE_MINUS_DST_ALPHA = 11,
	XSDK_BLEND_SRC_ALPHA_SATURATE = 16,
};

enum {
	XSDK_BLEND_OP_ADD,
	XSDK_BLEND_OP_SUBTRACT,
	XSDK_BLEND_OP_MIN,
	XSDK_BLEND_OP_MAX,
	XSDK_BLEND_OP_REVERSE_SUBTRACT,
};

// Colour write mask bits (xenia:src/xenia/gpu/registers.h:755-761).
enum {
	XSDK_WRITE_RED = 1 << 0,
	XSDK_WRITE_GREEN = 1 << 1,
	XSDK_WRITE_BLUE = 1 << 2,
	XSDK_WRITE_ALPHA = 1 << 3,
	XSDK_WRITE_ALL = 0xF,
};

// Source times source factor, op, destination times destination factor; ONE, ZERO and ADD both ways is no blending.
typedef struct {
	uint32_t color_source;
	uint32_t color_dest;
	uint32_t color_op;
	uint32_t alpha_source;
	uint32_t alpha_dest;
	uint32_t alpha_op;
	uint32_t write_mask;
} XsdkBlend;

typedef struct {
	uint32_t blend_control;
	uint32_t color_mask;
} XsdkBlendState;

void xsdk_gpu_blend_state(XsdkBlendState *state, const XsdkBlend *setting);
// Applies to the draws after; xsdk_gpu_set_target turns blending off and writes every channel.
void xsdk_gpu_set_blend(const XsdkBlendState *state);

enum {
	XSDK_CULL_NONE,
	XSDK_CULL_FRONT,
	XSDK_CULL_BACK,
};

// Which faces draw, and a depth bias: a constant in depth units, plus slope times the polygon's depth slope per pixel.
typedef struct {
	uint32_t cull;
	bool front_clockwise;
	float depth_bias;
	float depth_bias_slope;
} XsdkRaster;

typedef struct {
	uint32_t mode_control;
	// Front scale and offset, then back.
	uint32_t polygon_offset[4];
} XsdkRasterState;

void xsdk_gpu_raster_state(XsdkRasterState *state, const XsdkRaster *setting);
// Applies to the draws after; xsdk_gpu_set_target culls nothing and biases nothing.
void xsdk_gpu_set_raster(const XsdkRasterState *state);
// Draws only to pixels x to x + width - 1 and y to y + height - 1; xsdk_gpu_set_target opens it to the whole target.
void xsdk_gpu_set_scissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
// Maps clip space to that rectangle in pixels and depth to min_depth..max_depth; xsdk_gpu_set_target covers the target.
void xsdk_gpu_set_viewport(float x, float y, float width, float height, float min_depth, float max_depth);

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
