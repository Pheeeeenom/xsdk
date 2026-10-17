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
	// Every triangle shares the first vertex (xenia:src/xenia/gpu/xenos.h:50).
	XSDK_PRIMITIVE_TRIANGLE_FAN = 5,
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
// Waits until the GPU has passed the fence, or returns STATUS_IO_TIMEOUT, looking at it between 1 ms sleeps.
NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms);
// Has xsdk_gpu_wait look every 5 us for this long before it sleeps, so a wait returns within microseconds of a fence
// that soon; 0, the default, as the spinning slows the thread sharing the waiter's core (docs/HARDWARE.md).
void xsdk_gpu_set_wait_spin(uint32_t microseconds);
// Whether the GPU has passed the fence, without waiting; safe in the graphics interrupt callback.
bool xsdk_gpu_fence_done(uint32_t fence);
// Queues writing value to the word at address, 4-byte aligned, once the GPU is done with everything before, as a
// fence is written; the CPU reads it as given.
void xsdk_gpu_write_word(uint32_t address, uint32_t value);
// Queues the command processor waiting until the word at address holds value as the CPU wrote it; nothing after it
// runs until then, so a word no one writes stops the GPU for good (docs/HARDWARE.md, "Queries and events").
void xsdk_gpu_wait_word(uint32_t address, uint32_t value);

// The GPU's clock: 64 bits, 498.75 a microsecond, from 0 at xsdk_gpu_open (docs/HARDWARE.md, "Queries and events").
enum {
	XSDK_GPU_CLOCK_WORDS = 3,
};

// Queues copying the clock into XSDK_GPU_CLOCK_WORDS words at address, once the GPU is idle if after_work, else as the
// command processor reaches it.
void xsdk_gpu_write_clock(uint32_t address, bool after_work);
// The clock from the words xsdk_gpu_write_clock wrote: its high word, its low word, then its high word again.
uint64_t xsdk_gpu_clock_value(volatile const uint32_t *words);
// The clock now, as the CPU reads it.
uint64_t xsdk_gpu_clock(void);

// Visibility queries, D3D's: the draws between a query's begin and end set bit id % 32 of a status word if any of them
// got past rasterizing and the coarse hierarchical depth test. A draw hidden behind others sets it too, so it tells
// whether draws could show, not whether a sample passed the depth test (docs/HARDWARE.md, "Occlusion queries,
// vulkan4xenon milestone 28"). A query's end writes its bit, set or clear, so an id can be begun again once its word is
// copied; PA_SC_VIZ_QUERY's id, which the begin sets to the packet's, picks the bit the draws set.
enum {
	XSDK_VISIBILITY_IDS = 64,
};

// Queues beginning query id, 0 to XSDK_VISIBILITY_IDS - 1, for the draws after; one query at a time.
void xsdk_gpu_begin_visibility(uint32_t id);
// Queues ending query id, then, once the GPU is idle, copying the status word holding its bit, ids 0 to 31's or 32 to
// 63's, to the word at address, as the CPU reads it; copied sooner, the word held the bits from before.
void xsdk_gpu_end_visibility(uint32_t id, uint32_t address);

// What the GPU layer has sent since xsdk_gpu_open, to measure a title by.
typedef struct {
	// Written into the command stream, which the ring runs a segment at a time.
	uint64_t dwords;
	uint32_t kicks;
	// Times the CPU found the ring or the stream's next block still in use, and how long it waited.
	uint32_t waits;
	uint64_t waited_us;
	// Frames handed to the kernel's swap by xsdk_present_end, the value its last fence is to write, and the value
	// there now.
	uint32_t swaps;
	uint32_t swap_fence;
	uint32_t swap_fence_landed;
	// Of those frames, the ones XAM drew the Guide in. XAM's commands run between the title's frames and draw in
	// eDRAM, so what a title keeps in eDRAM across xsdk_present_end is lost whenever this count goes up.
	uint32_t system_draws;
} XsdkGpuStats;

void xsdk_gpu_stats(XsdkGpuStats *stats);

// Write-combined physical memory for the GPU, in 64 KB pages, or NULL. Write it with volatile 32-bit stores.
volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes);
void xsdk_gpu_free(volatile uint32_t *memory);
// The address the GPU uses for memory from xsdk_gpu_alloc.
uint32_t xsdk_gpu_address(volatile const uint32_t *memory);
// Packets recorded into GPU memory ahead of time, to run from the ring as often as wanted.
typedef struct {
	volatile uint32_t *dwords;
	uint32_t address;
	uint32_t capacity_dwords;
	uint32_t count;
} XsdkCommandBuffer;

NTSTATUS xsdk_gpu_command_buffer_create(XsdkCommandBuffer *buffer, uint32_t capacity_dwords);
void xsdk_gpu_command_buffer_free(XsdkCommandBuffer *buffer);
// Until xsdk_gpu_record_end, GPU calls append to the buffer, not the ring; no fences, resolves or executes inside.
void xsdk_gpu_record_begin(XsdkCommandBuffer *buffer);
// Ends the recording, or fails with STATUS_BUFFER_TOO_SMALL, leaving the buffer empty, if it outgrew the buffer.
NTSTATUS xsdk_gpu_record_end(void);
// Queues the recorded packets from the ring, constant blocks as recorded; keep the buffer until a later fence.
void xsdk_gpu_execute(const XsdkCommandBuffer *buffer);
// Queues copying the eDRAM colour target to address as tiled k_8_8_8_8, then clearing eDRAM; width a multiple of 32.
void xsdk_gpu_resolve(uint32_t address, uint32_t width, uint32_t height, uint32_t clear_argb);
// The bytes such a resolve writes, in whole 32-row tiles (xenia:src/xenia/gpu/draw_util.cc:1055-1062).
uint32_t xsdk_gpu_resolve_bytes(uint32_t width, uint32_t height);
// Queues the same into a tiled 32-bit RGBA texture drawn at its size, its whole pitch, leaving its upload's layout.
void xsdk_gpu_resolve_texture(const XsdkTexture *texture, uint32_t clear_argb);
// The same for eDRAM's depth and stencil, into a tiled XSDK_FORMAT_D24S8 texture; it clears depth and stencil as the
// resolves above do, and colour never.
void xsdk_gpu_resolve_depth_texture(const XsdkTexture *texture);

enum {
	// A resolve copies and clears 8 by 8 pixels at a time (xenia:src/xenia/gpu/xenos.h:978-984).
	XSDK_RESOLVE_ALIGNMENT = 8,
};

// xsdk_gpu_resolve_texture for the pixels from (x, y), width by height, into the same texels, clearing only them: x
// and y multiples of XSDK_RESOLVE_ALIGNMENT, and width and height too unless the rectangle reaches the texture's edge.
// Clearing depth there turns hierarchical depth off till a clear of the whole.
void xsdk_gpu_resolve_texture_rect(
	const XsdkTexture *texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t clear_argb);
// Which of colour, and depth with stencil, the later resolves clear after copying; both until set.
void xsdk_gpu_set_resolve_clears(bool color, bool depth_stencil);

enum {
	// eDRAM's tiles of 80x16 samples at 32 bits (xenia:src/xenia/gpu/xenos.h:414-416).
	XSDK_EDRAM_TILES = 2048,
};

// Sets the eDRAM colour target and a viewport, no depth, blending or culling (libxenon). A resolve undoes it, and a
// draw after one, before the target is set again outside a recording, stops at an assert.
void xsdk_gpu_set_target(uint32_t width, uint32_t height);
// The same with colour from eDRAM tile color_tile and depth from depth_tile, not depth then colour; a resolve undoes
// it too, keeping only where the target sits.
void xsdk_gpu_set_target_at(uint32_t width, uint32_t height, uint32_t color_tile, uint32_t depth_tile);
// The eDRAM tiles a target this size takes for its colour, and as many again for its depth.
uint32_t xsdk_gpu_target_tiles(uint32_t width, uint32_t height);
// The three above with 1, 2 or 4 samples a pixel, where they take 1; resolves average a pixel's samples.
void xsdk_gpu_set_target_samples(uint32_t width, uint32_t height, uint32_t samples);
void xsdk_gpu_set_target_samples_at(
	uint32_t width, uint32_t height, uint32_t samples, uint32_t color_tile, uint32_t depth_tile);
uint32_t xsdk_gpu_target_sample_tiles(uint32_t width, uint32_t height, uint32_t samples);

// Colour target formats (xenia:src/xenia/gpu/xenos.h:299-313); a 64-bit one takes twice the tiles (xenos.h:321-326,
// 427-437). The set above draws 8_8_8_8.
enum {
	XSDK_TARGET_8_8_8_8 = 0,
	XSDK_TARGET_2_10_10_10 = 2,
	// 7e3 floats from 0 to 31.875 in red, green and blue, and alpha in 2 bits.
	XSDK_TARGET_2_10_10_10_FLOAT = 3,
	// Fixed point from -32 to 32.
	XSDK_TARGET_16_16_16_16 = 5,
	XSDK_TARGET_16_16_FLOAT = 6,
	XSDK_TARGET_16_16_16_16_FLOAT = 7,
	XSDK_TARGET_32_FLOAT = 14,
};

enum {
	// Colour targets drawn at once, target i from oCi (xenia:src/xenia/gpu/xenos.h:297; ucode.h:1836-1839).
	XSDK_COLOR_TARGETS = 4,
};

// A colour target: its format and the eDRAM tile it starts at.
typedef struct {
	uint32_t format;
	uint32_t tile;
} XsdkColorTarget;

// The eDRAM tiles a colour target this size and format takes.
uint32_t xsdk_gpu_color_target_tiles(uint32_t width, uint32_t height, uint32_t format);
// As xsdk_gpu_set_target_at, with 1 to XSDK_COLOR_TARGETS colour targets, each of its own format and tile, apart in
// eDRAM; each writes every channel, unblended, till xsdk_gpu_set_blends.
void xsdk_gpu_set_targets(
	uint32_t width, uint32_t height, const XsdkColorTarget *targets, uint32_t count, uint32_t depth_tile);
// Queues copying colour target `target` of the last xsdk_gpu_set_targets into a tiled texture drawn at its size, its
// whole pitch, converted to the texture's format, then clearing the target to clear_rgba as xsdk_gpu_set_resolve_clears
// says. 8_8_8_8 goes into A8R8G8B8, R8G8B8A8 or B8G8R8A8; 2_10_10_10 into A2B10G10R10; 16_16_FLOAT into R16G16F;
// 32_FLOAT into R32F; the 7e3 one and the 16-bit fixed and float ones of four channels into R16G16B16A16F.
void xsdk_gpu_resolve_target(uint32_t target, const XsdkTexture *texture, const float clear_rgba[4]);

// Loads both shaders with IM_LOAD and sets the program registers; a resolve replaces the vertex shader.
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel);
// After the CPU writes vertices, indices or texels, xsdk_texture_upload's included, and before the draws that read
// them: the GPU's caches drop what they hold of that memory, as libxenon's unlock does. A resolve does it for itself.
void xsdk_gpu_invalidate(uint32_t address, uint32_t size_bytes);
// Points a vertex stream at vertices from xsdk_gpu_alloc; streams 0-2 share texture slot 31's group, 3-5 slot 30's.
void xsdk_gpu_set_vertices(uint32_t stream, uint32_t address, uint32_t size_bytes);
// Points fetch constant slot 0 to 31 at the texture and sampler; a resolve takes slot 0, vertex streams 31 down.
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

// One face's stencil test, what it writes, and its reference and masks.
typedef struct {
	uint32_t compare;
	uint32_t fail;
	uint32_t depth_fail;
	uint32_t pass;
	uint8_t reference;
	uint8_t read_mask;
	uint8_t write_mask;
} XsdkStencilFace;

// How draws test and write the target's 24-bit depth and 8-bit stencil. The stencil fields serve both faces, unless
// two_sided gives back faces their own; lines and points are front faces (xenia:src/xenia/gpu/registers.h:799-834).
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
	bool two_sided;
	XsdkStencilFace back;
} XsdkDepthStencil;

// The registers a depth/stencil setting writes, worked out once.
typedef struct {
	uint32_t depth_control;
	uint32_t stencil_ref_mask;
	uint32_t stencil_ref_mask_back;
} XsdkDepthStencilState;

void xsdk_gpu_depth_stencil_state(XsdkDepthStencilState *state, const XsdkDepthStencil *setting);
// Applies to the draws after; xsdk_gpu_set_target and resolves turn depth and stencil off. Draws testing depth skip
// hidden pixels before the pixel shader from a resolve that clears the whole target's depth until another target is
// set, a recording is begun or run, part of the depth is cleared, or depth is written with a compare the clear doesn't
// suit (docs/HARDWARE.md, "Hierarchical depth"); draws whose pixel shader writes depth never do.
void xsdk_gpu_set_depth_stencil(const XsdkDepthStencilState *state);
// The depth, 0 to 1, and stencil the later resolves clear to; far and 0 until set. Below 0.5 suits greater-than
// compares, else less-than ones; equal suits both.
void xsdk_gpu_set_depth_clear(float depth, uint8_t stencil);

// Blend factors (xenia:src/xenia/gpu/xenos.h:708-723), the constant ones taking xsdk_gpu_set_blend_constants's colour,
// and operations (727-733).
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
	XSDK_BLEND_CONSTANT_COLOR = 12,
	XSDK_BLEND_ONE_MINUS_CONSTANT_COLOR = 13,
	XSDK_BLEND_CONSTANT_ALPHA = 14,
	XSDK_BLEND_ONE_MINUS_CONSTANT_ALPHA = 15,
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
// Applies to the draws after, the first colour target alone, the others then written nowhere; xsdk_gpu_set_target turns
// blending off and writes every channel.
void xsdk_gpu_set_blend(const XsdkBlendState *state);
// The same for count colour targets, target i blended and masked by states[i] (RB_BLENDCONTROL0 to 3,
// xenia:src/xenia/gpu/register_table.inc:537-547; RB_COLOR_MASK, registers.h:755-777); the targets past count are
// written nowhere.
void xsdk_gpu_set_blends(const XsdkBlendState *states, uint32_t count);
// The colour the constant factors take, red to alpha, for the draws after; nothing else sets it, so a draw blending by
// it sets it first (docs/HARDWARE.md, "Blend constants").
void xsdk_gpu_set_blend_constants(const float rgba[4]);

enum {
	XSDK_CULL_NONE,
	XSDK_CULL_FRONT,
	XSDK_CULL_BACK,
};

// How triangles that aren't culled draw: filled, as their edges' lines, or as their corners' points, 1 pixel each
// (xenia:src/xenia/gpu/registers.h:463-469; xenos.h:846-859).
enum {
	XSDK_FILL_SOLID,
	XSDK_FILL_WIREFRAME,
	XSDK_FILL_POINTS,
};

// Which faces draw and how, and a depth bias: a constant in depth units, plus slope times the polygon's depth slope per
// pixel.
typedef struct {
	uint32_t cull;
	bool front_clockwise;
	float depth_bias;
	float depth_bias_slope;
	uint32_t fill;
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

enum {
	// Xenos clips against six planes (xenia:src/xenia/gpu/registers.h:539-544).
	XSDK_CLIP_PLANES = 6,
};

// Clips the draws after to where dot(the vertex's clip-space position, plane n) >= 0, for each bit n of mask, plane n
// planes[n]'s x, y, z and w, as Xenia applies them (xenia:src/xenia/gpu/dxbc_shader_translator.cc:1033-1053); planes
// may be NULL for a mask of 0. xsdk_gpu_set_target and resolves turn every plane off.
void xsdk_gpu_set_clip_planes(uint32_t mask, const float planes[][4]);

// Shader constants for the draws after, written from a copy: float constants in whole groups of 4, the boolean and
// loop words all together, as lone ones hung the GPU (docs/HARDWARE.md).
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
// Has the GPU load count float constants from first straight from memory at address, as the console takes them:
// whole groups of 4 from a multiple of 4, from 64-byte aligned memory (docs/HARDWARE.md). The setters' copy doesn't
// hold loaded values, so a group that's loaded mustn't also be set.
void xsdk_gpu_load_vertex_constants(uint32_t first, uint32_t address, uint32_t count);
void xsdk_gpu_load_pixel_constants(uint32_t first, uint32_t address, uint32_t count);
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
