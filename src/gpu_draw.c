// The draw state below comes from libxenon:drivers/xenos/xe.c, rewritten with Xenia's register names.
// libxenon carries this notice:
//
// Copyright (c) 2009, free60.org
// USB Stack Copyright (c) 2000,2001,2002,2003,2005, Broadcom Corporation
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the copyright holder nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY free60.org ''AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL free60.org BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE

#include "xsdk/gpu.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "gpu_private.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

enum {
	DRAW_MAX_DWORDS = 96,
	// What each call that runs per draw writes, in place: a type-0 packet is its header and a dword a register.
	IM_LOAD_DWORDS = 3,
	PROGRAM_DWORDS = 1 + 3,
	VERTICES_DWORDS = 1 + 6 + 1 + 3,
	TEXTURE_DWORDS = 1 + 6,
	DRAW_DWORDS = 2,
	DRAW_INDEXED_DWORDS = 5,
	DEPTH_STENCIL_DWORDS = 3 * 2,
	BLEND_DWORDS = 2 * 2,
	RASTER_DWORDS = 2 + 1 + 4,
	SCISSOR_DWORDS = 1 + 2,
	VIEWPORT_DWORDS = 1 + 6,
	// eDRAM tiles of samples (xenia:src/xenia/gpu/xenos.h:414-416).
	TILE_WIDTH = 80,
	TILE_HEIGHT = 16,
	// A resolve's width is whole columns of 32 pixels.
	RESOLVE_COLUMN = 32,
	// RB_SURFACE_INFO's fields (xenia:src/xenia/gpu/registers.h:670-679).
	SURFACE_SAMPLES_SHIFT = 16,
	SURFACE_HIZ_PITCH_SHIFT = 18,
	EDRAM_MODE_COLOR_DEPTH = 4,
	// libxenon's control state from RB_DEPTHCONTROL on, less depth and culling (xe.c:1403-1416).
	BLEND_ONE_ZERO = 0x00010001,
	COLOR_CONTROL = 0x87000007,
	CLIP_D3D_SPACE = 0x00080000,
	SC_MODE_WINDOW_OFFSET_NO_CULL = 0x00010000,
	VTE_VIEWPORT_AND_W0 = 0x0000043F,
	STENCIL_MASKS = 0x00FFFF00,
	INTERPOLATE_ALL_AT_CENTERS = 0xFFFFFFFF,
	// SQ_CONTEXT_MISC's sc_sample_cntl (xenia:src/xenia/gpu/registers.h:157; xenos.h:804-808).
	SAMPLE_CNTL_MASK = 3 << 2,
	SAMPLE_CNTL_CENTERS_ONLY = 1 << 2,
	MAX_VERTEX_INDEX = 0x00FFFFFF,
	// Stream 0's vertex fetch constant and libxenon's fetch word: type 3, k8in32, bit 28 (xe.c:1181-1185, 1965).
	VERTEX_FETCH_CONSTANT = 95,
	// Vertex fetch constants go in threes (xenia:src/xenia/gpu/xenos.h:1288-1315).
	FETCHES_PER_GROUP = 3,
	VERTEX_FETCH_DWORDS = 2,
	FETCH_GROUP_DWORDS = 6,
	// The dword of a texture fetch constant whose levels its texture and sampler both clamp (src/texture.c).
	TEXTURE_LEVELS_DWORD = 4,
	LAST_GROUP = VERTEX_FETCH_CONSTANT / FETCHES_PER_GROUP,
	STREAM_GROUPS = LAST_GROUP - (VERTEX_FETCH_CONSTANT - (XSDK_VERTEX_STREAMS - 1)) / FETCHES_PER_GROUP + 1,
	// A texture fetch takes a 5-bit fetch constant index (xenia:src/xenia/gpu/ucode.h:789-790).
	TEXTURE_SLOTS = 32,
	VERTEX_FETCH_TYPE = 3,
	VERTEX_FETCH_8IN32 = 1 << 28 | 2,
	VERTEX_FETCH_SIZE_SHIFT = 2,
	AUTO_INDEX = 2 << 6,
	VERTEX_COUNT_SHIFT = 16,
	// The initiator's index_size bit and the size's 8-in-16 swap (xenia:src/xenia/gpu/registers.h:300-327).
	INDEX_32_BIT_SHIFT = 11,
	DMA_SWAP_8IN16 = 1 << 30,
	MAX_INDICES = 0xFFFF,
	// RB_DEPTHCONTROL's fields and RB_STENCILREFMASK's bytes (xenia:src/xenia/gpu/registers.h:799-834).
	STENCIL_ENABLE = 1 << 0,
	Z_ENABLE = 1 << 1,
	Z_WRITE_ENABLE = 1 << 2,
	ZFUNC_SHIFT = 4,
	STENCILFUNC_SHIFT = 8,
	STENCILFAIL_SHIFT = 11,
	STENCILZPASS_SHIFT = 14,
	STENCILZFAIL_SHIFT = 17,
	STENCILMASK_SHIFT = 8,
	STENCILWRITEMASK_SHIFT = 16,
	COMPARE_MASK = 7,
	STENCIL_FAIL_OPS = 7 << STENCILFAIL_SHIFT | 7 << STENCILZFAIL_SHIFT,
	// RB_DEPTH_CLEAR's 0.5, depth above the stencil.
	DEPTH_CLEAR_HALF = 0x80000000,
	// RB_BLENDCONTROL's fields (xenia:src/xenia/gpu/registers.h:780-794).
	COLOR_COMB_FCN_SHIFT = 5,
	COLOR_DESTBLEND_SHIFT = 8,
	ALPHA_SRCBLEND_SHIFT = 16,
	ALPHA_COMB_FCN_SHIFT = 21,
	ALPHA_DESTBLEND_SHIFT = 24,
	// PA_SU_SC_MODE_CNTL's fields (registers.h:456-486); libxenon keeps the window offset on (xe.c:1403-1416).
	CULL_FRONT = 1 << 0,
	CULL_BACK = 1 << 1,
	FACE_CLOCKWISE = 1 << 2,
	POLY_OFFSET_FRONT_ENABLE = 1 << 11,
	POLY_OFFSET_BACK_ENABLE = 1 << 12,
	MSAA_ENABLE = 1 << 15,
	VTX_WINDOW_OFFSET_ENABLE = 1 << 16,
	// The slope scale register counts 1/16-pixel subpixels (xenia:src/xenia/gpu/xenos.h:404-407).
	SUBPIXELS_PER_PIXEL = 16,
	SCISSOR_Y_SHIFT = 16,
};

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// PA_SC_AA_CONFIG: the samples less one, and the farthest from a pixel's centre in 16ths from bit 13
// (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1540-1543); libxenon's 2 and 4 fail (docs/HARDWARE.md).
static const uint32_t aa_configs[] = { 0, 0x00008001, 0x0000C003 };
// RB_COPY_CONTROL's copy_sample_select: sample 0, or the average of samples 0-1 or 0-3
// (xenia:src/xenia/gpu/registers.h:858; xenos.h:1000-1008), as libxenon picks them (xe.c:1096-1105).
static const uint32_t copy_sample_selects[] = { 0, 4, 6 };

// The last target's samples a pixel, and where xsdk_gpu_set_target_samples_at put its colour and depth, in eDRAM tiles.
static uint32_t target_samples = 1;
static bool target_placed;
static uint32_t placed_color_tile;
static uint32_t placed_depth_tile;
// A resolve leaves eDRAM copying with the viewport transform off, where a draw hung the GPU (docs/HARDWARE.md).
static bool target_undone;

typedef struct {
	uint32_t pitch_tiles;
	uint32_t height;
	uint32_t samples;
	uint32_t depth_tile;
} Surface;

// Whether the hierarchical buffer bounds the depth of the surface it was last cleared for, and from which side.
static bool hiz_kept;
static bool hiz_greater;
static Surface hiz_surface;

// MsaaSamples, 0 to 2 for 1, 2 and 4 samples (xenia:src/xenia/gpu/xenos.h:288-292).
static uint32_t samples_log2(uint32_t samples)
{
	return samples / 2;
}

// A pixel's 2 samples sit one above the other, its 4 two by two (xenia:src/xenia/gpu/shaders/edram.xesli:27-32).
static uint32_t samples_across(uint32_t samples)
{
	return samples == 4 ? 2 : 1;
}

// An even number of tiles across (xe.c:998-1036), over whole resolve columns, so resolving a texture's pitch reads the
// rows where its draws put them.
static uint32_t tiles_across(uint32_t width, uint32_t samples)
{
	const uint32_t columns = (width + RESOLVE_COLUMN - 1) & ~(uint32_t)(RESOLVE_COLUMN - 1);
	return ((columns * samples_across(samples) + TILE_WIDTH - 1) / TILE_WIDTH + 1) & ~1u;
}

uint32_t xsdk_gpu_target_sample_tiles(uint32_t width, uint32_t height, uint32_t samples)
{
	assert(samples == 1 || samples == 2 || samples == 4);
	const uint32_t rows = height * (samples / samples_across(samples));
	return tiles_across(width, samples) * ((rows + TILE_HEIGHT - 1) / TILE_HEIGHT);
}

static Surface surface_of(uint32_t width, uint32_t height)
{
	const uint32_t depth_tile = target_placed ? placed_depth_tile : 0;
	return (Surface){ tiles_across(width, target_samples), height, target_samples, depth_tile };
}

static bool is_hiz_surface(uint32_t width, uint32_t height)
{
	const Surface surface = surface_of(width, height);
	return surface.pitch_tiles == hiz_surface.pitch_tiles && surface.height == hiz_surface.height &&
		surface.samples == hiz_surface.samples && surface.depth_tile == hiz_surface.depth_tile;
}

void xsdk_gpu_hiz_cleared(uint32_t width, uint32_t height, uint32_t depth_clear)
{
	hiz_surface = surface_of(width, height);
	// Multisampled targets haven't been tried with the buffer on the console.
	hiz_kept = target_samples == 1;
	// Reversed depth clears to 0 and tests greater-than.
	hiz_greater = depth_clear < DEPTH_CLEAR_HALF;
}

void xsdk_gpu_hiz_drop(void)
{
	hiz_kept = false;
}

uint32_t xsdk_gpu_target_tiles(uint32_t width, uint32_t height)
{
	return xsdk_gpu_target_sample_tiles(width, height, 1);
}

uint32_t xsdk_gpu_copy_sample_select(void)
{
	return copy_sample_selects[samples_log2(target_samples)];
}

// As libxenon sets it (xe.c:1411-1412): without msaa_enable the console's edges don't blend (docs/HARDWARE.md).
static uint32_t msaa_enable(void)
{
	return target_samples > 1 ? MSAA_ENABLE : 0;
}

// Xe_SetRenderTarget's layout unless placed: depth then colour, as many tiles each (xe.c:998-1036).
void xsdk_gpu_put_surface(GpuPackets *packets, uint32_t width, uint32_t height)
{
	const uint32_t target_tiles = xsdk_gpu_target_sample_tiles(width, height, target_samples);
	const uint32_t color_tile = target_placed ? placed_color_tile : target_tiles;
	const uint32_t depth_tile = target_placed ? placed_depth_tile : 0;
	assert(color_tile + target_tiles <= XSDK_EDRAM_TILES && depth_tile + target_tiles <= XSDK_EDRAM_TILES);
	// The pitch counts pixels (xenia:src/xenia/gpu/xenos.h:426-437).
	const uint32_t edram_pitch = tiles_across(width, target_samples) * TILE_WIDTH / samples_across(target_samples);
	const uint32_t surface[] = { edram_pitch | samples_log2(target_samples) << SURFACE_SAMPLES_SHIFT |
			edram_pitch << SURFACE_HIZ_PITCH_SHIFT,
		color_tile, depth_tile };
	gpu_put_registers(packets, RB_SURFACE_INFO, surface, 3);
	// Resolves too take the target's samples here, else they miss pixels or copy zeros (docs/HARDWARE.md).
	gpu_put_register(packets, PA_SC_AA_CONFIG, aa_configs[samples_log2(target_samples)]);
	gpu_put_register(packets, PA_SC_AA_MASK, 0xFFFF);
}

// Xe_pSetState's control and miscellaneous state (xe.c:1688-1744).
static void put_target(uint32_t width, uint32_t height)
{
	const uint32_t control[] = { 0, BLEND_ONE_ZERO, COLOR_CONTROL, 0, CLIP_D3D_SPACE,
		SC_MODE_WINDOW_OFFSET_NO_CULL | msaa_enable(), VTE_VIEWPORT_AND_W0, 0, EDRAM_MODE_COLOR_DEPTH };
	// The index range, and the offset added to each index: 0, set here rather than before every draw as libxenon
	// does (xe.c:191-200), as every draw has a target set after anything that could change it.
	const uint32_t indices[] = { MAX_VERTEX_INDEX, 0, 0 };
	// The stencil masks, alpha reference 0, then a viewport over the target with z from 0 to 1.
	const uint32_t viewport[] = { STENCIL_MASKS, STENCIL_MASKS, 0, float_bits(width / 2.0f), float_bits(width / 2.0f),
		float_bits(-(float)height / 2.0f), float_bits(height / 2.0f), float_bits(1.0f), 0 };
	// Xe_pStuff's zeros before every draw (xe.c:1233-1238).
	const uint32_t polygon_offset[8] = { 0 };
	// The buffer is laid out by the surface, which another target's depth may also overlap.
	if (!is_hiz_surface(width, height))
		hiz_kept = false;
	uint32_t dwords[DRAW_MAX_DWORDS];
	GpuPackets packets = { .dwords = dwords, .count = 0, .capacity = DRAW_MAX_DWORDS };
	gpu_put_registers(&packets, PA_SU_POLY_OFFSET_FRONT_SCALE, polygon_offset, 8);
	gpu_put_registers(&packets, RB_DEPTHCONTROL, control, 9);
	xsdk_gpu_put_window(&packets, width, height);
	xsdk_gpu_put_surface(&packets, width, height);
	gpu_put_register(&packets, COHER_DEST_BASE_1, 0);
	gpu_put_register(&packets, COHER_DEST_BASE_7, 0);
	gpu_put_registers(&packets, VGT_MAX_VTX_INDX, indices, 3);
	gpu_put_register(&packets, RB_COLOR_MASK, 0xF);
	gpu_put_registers(&packets, RB_STENCILREFMASK_BF, viewport, 9);
	xsdk_gpu_write(&packets);
	// A recorded target is set when its buffer runs, not now.
	if (!xsdk_gpu_recording())
		target_undone = false;
}

void xsdk_gpu_undo_target(void)
{
	target_undone = true;
}

void xsdk_gpu_set_target_samples(uint32_t width, uint32_t height, uint32_t samples)
{
	assert(samples == 1 || samples == 2 || samples == 4);
	target_samples = samples;
	target_placed = false;
	put_target(width, height);
}

void xsdk_gpu_set_target(uint32_t width, uint32_t height)
{
	xsdk_gpu_set_target_samples(width, height, 1);
}

// A base is any tile: Xenia adds it to each pixel's tile, wrapping (xenia:src/xenia/gpu/draw_util.cc:1111-1138).
void xsdk_gpu_set_target_samples_at(
	uint32_t width, uint32_t height, uint32_t samples, uint32_t color_tile, uint32_t depth_tile)
{
	assert(samples == 1 || samples == 2 || samples == 4);
	target_samples = samples;
	target_placed = true;
	placed_color_tile = color_tile;
	placed_depth_tile = depth_tile;
	put_target(width, height);
}

void xsdk_gpu_set_target_at(uint32_t width, uint32_t height, uint32_t color_tile, uint32_t depth_tile)
{
	xsdk_gpu_set_target_samples_at(width, height, 1, color_tile, depth_tile);
}

// IM_LOAD takes address | type, then the size in dwords (xenia:src/xenia/gpu/command_processor.cc:1535-1539).
static void put_im_load(GpuPackets *packets, const XsdkShader *shader)
{
	gpu_put_type3(packets, PM4_IM_LOAD, 2);
	gpu_put(packets, shader->address | shader->type);
	gpu_put(packets, shader->dword_count);
}

// The microcode the GPU holds, by address, so a shader loaded isn't loaded again; 0 when it isn't known. Setting the
// same program again cost the GPU 0.11 us a draw, and loading both shaders where only the pixel shader changed 0.07
// us more than loading it alone (docs/HARDWARE.md, "The GPU's cost of a draw").
static uint32_t loaded_vertex;
static uint32_t loaded_pixel;

void xsdk_gpu_forget_shaders(void)
{
	loaded_vertex = 0;
	loaded_pixel = 0;
}

// Xe_pUploadShader: the pixel shader first, then the vertex shader and the program registers (xe.c:1378-1401), each
// shader only if it isn't the one loaded, and nothing if neither changed.
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel)
{
	assert(vertex->type == XSDK_SHADER_VERTEX && pixel->type == XSDK_SHADER_PIXEL);
	const bool load_vertex = vertex->address != loaded_vertex;
	const bool load_pixel = pixel->address != loaded_pixel;
	if (!load_vertex && !load_pixel)
		return;
	// Every interpolant is at its centre, and centroids and centres halve the pixel rate (docs/HARDWARE.md).
	const uint32_t context_misc =
		((vertex->context_misc | pixel->context_misc) & ~(uint32_t)SAMPLE_CNTL_MASK) | SAMPLE_CNTL_CENTERS_ONLY;
	const uint32_t program[] = { vertex->program_control | pixel->program_control, context_misc,
		INTERPOLATE_ALL_AT_CENTERS };
	GpuPackets packets =
		gpu_reserve_packets((load_pixel ? IM_LOAD_DWORDS : 0) + (load_vertex ? IM_LOAD_DWORDS : 0) + PROGRAM_DWORDS);
	if (load_pixel)
		put_im_load(&packets, pixel);
	if (load_vertex)
		put_im_load(&packets, vertex);
	gpu_put_registers(&packets, SQ_PROGRAM_CNTL, program, 3);
	gpu_packets_done(&packets);
	loaded_vertex = vertex->address;
	loaded_pixel = pixel->address;
}

// The streams' vertex fetch constants, 95 down, kept so each write can send its whole group.
static uint32_t vertex_groups[STREAM_GROUPS][FETCH_GROUP_DWORDS];

void xsdk_gpu_set_vertices(uint32_t stream, uint32_t address, uint32_t size_bytes)
{
	assert(stream < XSDK_VERTEX_STREAMS);
	const uint32_t constant = VERTEX_FETCH_CONSTANT - stream;
	const uint32_t group_index = constant / FETCHES_PER_GROUP;
	uint32_t *const group = vertex_groups[LAST_GROUP - group_index];
	const uint32_t slot = constant % FETCHES_PER_GROUP * VERTEX_FETCH_DWORDS;
	const uint32_t base = address | VERTEX_FETCH_TYPE;
	const uint32_t size = VERTEX_FETCH_8IN32 | size_bytes / 4 << VERTEX_FETCH_SIZE_SHIFT;
	const uint32_t flush_fetch[] = { 0, 0, 0 };
	GpuPackets packets = gpu_reserve_packets(VERTICES_DWORDS);
	// libxenon flushes the fetch constants it set (xe.c:1146).
	// Written alone, a fetch constant breaks drawing (docs/HARDWARE.md, "Fetch constants go in whole groups"). The
	// group's others are as kept; this one is kept after it's written, so nothing just stored is read back.
	gpu_put_type0(&packets, SHADER_CONSTANT_FETCH_00_0 + group_index * FETCH_GROUP_DWORDS, FETCH_GROUP_DWORDS);
	for (uint32_t i = 0; i < FETCH_GROUP_DWORDS; i++)
		gpu_put(&packets, i == slot ? base : i == slot + 1 ? size : group[i]);
	gpu_put_registers(&packets, SHADER_CONSTANT_FLUSH_FETCH_0, flush_fetch, 3);
	gpu_packets_done(&packets);
	group[slot] = base;
	group[slot + 1] = size;
}

// Both caches, as Xe_SetTexture invalidates them (xe.c:1748); once per write, not every draw, as libxenon's unlock
// invalidates (xe.c:750-761).
void xsdk_gpu_invalidate(uint32_t address, uint32_t size_bytes)
{
	GpuPackets packets = gpu_reserve_packets(GPU_COHERENCY_DWORDS);
	xsdk_gpu_put_coherency(&packets, COHER_VERTEX_CACHE | COHER_TEXTURE_CACHE, address, size_bytes);
	gpu_packets_done(&packets);
}

// Xe_SetTexture's fetch constant, as a whole group (xe.c:1748), without the fetch constant flush libxenon writes after
// it: textures changed every draw came out right without it, and it cost the GPU 0.03 us a change (docs/HARDWARE.md,
// "The GPU's cost of a draw").
void xsdk_gpu_set_texture(uint32_t slot, const XsdkTexture *texture, const XsdkSamplerState *sampler)
{
	assert(slot < TEXTURE_SLOTS);
	GpuPackets packets = gpu_reserve_packets(TEXTURE_DWORDS);
	gpu_put_type0(&packets, SHADER_CONSTANT_FETCH_00_0 + slot * FETCH_GROUP_DWORDS, FETCH_GROUP_DWORDS);
	for (uint32_t i = 0; i < FETCH_GROUP_DWORDS; i++)
		gpu_put(&packets,
			i == TEXTURE_LEVELS_DWORD ? xsdk_texture_levels(texture->fetch[i], sampler->fetch[i])
									  : texture->fetch[i] | sampler->fetch[i]);
	gpu_packets_done(&packets);
}

// Xe_DrawPrimitive, with DRAW_INDX_2 as the resolve uses it (xe.c:1943-1957).
void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count)
{
	assert(!target_undone || xsdk_gpu_recording());
	GpuPackets packets = gpu_reserve_packets(DRAW_DWORDS);
	gpu_put_type3(&packets, PM4_DRAW_INDX_2, 1);
	gpu_put(&packets, primitive_type | AUTO_INDEX | vertex_count << VERTEX_COUNT_SHIFT);
	gpu_packets_done(&packets);
}

void xsdk_gpu_depth_stencil_state(XsdkDepthStencilState *state, const XsdkDepthStencil *setting)
{
	assert(setting->depth_compare <= XSDK_COMPARE_ALWAYS && setting->stencil_compare <= XSDK_COMPARE_ALWAYS);
	state->depth_control = (setting->stencil_test ? STENCIL_ENABLE : 0) | (setting->depth_test ? Z_ENABLE : 0) |
		(setting->depth_write ? Z_WRITE_ENABLE : 0) | setting->depth_compare << ZFUNC_SHIFT |
		setting->stencil_compare << STENCILFUNC_SHIFT | setting->stencil_fail << STENCILFAIL_SHIFT |
		setting->stencil_pass << STENCILZPASS_SHIFT | setting->stencil_depth_fail << STENCILZFAIL_SHIFT;
	state->stencil_ref_mask = setting->stencil_reference | (uint32_t)setting->stencil_read_mask << STENCILMASK_SHIFT |
		(uint32_t)setting->stencil_write_mask << STENCILWRITEMASK_SHIFT;
}

// What the hierarchical buffer can do for this setting; a depth write it can't follow drops it (docs/HARDWARE.md).
static uint32_t hiz_control(uint32_t depth_control)
{
	const uint32_t compare = depth_control >> ZFUNC_SHIFT & COMPARE_MASK;
	// Less-than's never pass a pixel farther than the buffer holds, nor greater-than's, the even ones, a nearer one.
	const bool follows =
		(depth_control & Z_ENABLE) && (hiz_greater ? (compare & 1) == 0 : compare < XSDK_COMPARE_GREATER);
	const bool writes = depth_control & Z_WRITE_ENABLE;
	if (writes && !follows)
		hiz_kept = false;
	if (!hiz_kept || !follows)
		return 0;
	// A pixel the buffer rejects skips the stencil test, and with it the fail operations.
	const bool stencil_kept = !(depth_control & STENCIL_ENABLE) || !(depth_control & STENCIL_FAIL_OPS);
	return (hiz_greater ? HIZ_GREATER : 0) | (writes ? HIZ_WRITE_ENABLE : 0) | (stencil_kept ? HIZ_ENABLE : 0);
}

// With backface_enable clear, the front face's stencil setting serves both faces, as D3D's one-sided stencil does.
void xsdk_gpu_set_depth_stencil(const XsdkDepthStencilState *state)
{
	GpuPackets packets = gpu_reserve_packets(DEPTH_STENCIL_DWORDS);
	gpu_put_register(&packets, RB_DEPTHCONTROL, state->depth_control);
	gpu_put_register(&packets, RB_HIZCONTROL, hiz_control(state->depth_control));
	gpu_put_register(&packets, RB_STENCILREFMASK, state->stencil_ref_mask);
	gpu_packets_done(&packets);
}

void xsdk_gpu_blend_state(XsdkBlendState *state, const XsdkBlend *setting)
{
	assert(setting->color_op <= XSDK_BLEND_OP_REVERSE_SUBTRACT && setting->alpha_op <= XSDK_BLEND_OP_REVERSE_SUBTRACT);
	state->blend_control = setting->color_source | setting->color_op << COLOR_COMB_FCN_SHIFT |
		setting->color_dest << COLOR_DESTBLEND_SHIFT | setting->alpha_source << ALPHA_SRCBLEND_SHIFT |
		setting->alpha_op << ALPHA_COMB_FCN_SHIFT | setting->alpha_dest << ALPHA_DESTBLEND_SHIFT;
	state->color_mask = setting->write_mask & XSDK_WRITE_ALL;
}

void xsdk_gpu_set_blend(const XsdkBlendState *state)
{
	GpuPackets packets = gpu_reserve_packets(BLEND_DWORDS);
	gpu_put_register(&packets, RB_BLENDCONTROL0, state->blend_control);
	gpu_put_register(&packets, RB_COLOR_MASK, state->color_mask);
	gpu_packets_done(&packets);
}

void xsdk_gpu_raster_state(XsdkRasterState *state, const XsdkRaster *setting)
{
	const bool biased = setting->depth_bias != 0.0f || setting->depth_bias_slope != 0.0f;
	state->mode_control = VTX_WINDOW_OFFSET_ENABLE | (setting->front_clockwise ? FACE_CLOCKWISE : 0) |
		(setting->cull == XSDK_CULL_FRONT ? CULL_FRONT : 0) | (setting->cull == XSDK_CULL_BACK ? CULL_BACK : 0) |
		(biased ? POLY_OFFSET_FRONT_ENABLE | POLY_OFFSET_BACK_ENABLE : 0);
	const uint32_t scale = float_bits(setting->depth_bias_slope * SUBPIXELS_PER_PIXEL);
	const uint32_t offset = float_bits(setting->depth_bias);
	state->polygon_offset[0] = scale;
	state->polygon_offset[1] = offset;
	state->polygon_offset[2] = scale;
	state->polygon_offset[3] = offset;
}

void xsdk_gpu_set_raster(const XsdkRasterState *state)
{
	GpuPackets packets = gpu_reserve_packets(RASTER_DWORDS);
	gpu_put_register(&packets, PA_SU_SC_MODE_CNTL, state->mode_control | msaa_enable());
	gpu_put_registers(&packets, PA_SU_POLY_OFFSET_FRONT_SCALE, state->polygon_offset, 4);
	gpu_packets_done(&packets);
}

// The window offset stays 0: target pixels, bottom right exclusive (xenia:src/xenia/gpu/draw_util.cc:559-607).
void xsdk_gpu_set_scissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	const uint32_t scissor[] = { x | y << SCISSOR_Y_SHIFT, (x + width) | (y + height) << SCISSOR_Y_SHIFT };
	GpuPackets packets = gpu_reserve_packets(SCISSOR_DWORDS);
	gpu_put_registers(&packets, PA_SC_WINDOW_SCISSOR_TL, scissor, 2);
	gpu_packets_done(&packets);
}

// The scale and offset xsdk_gpu_set_target writes for the whole target, moved to the rectangle.
void xsdk_gpu_set_viewport(float x, float y, float width, float height, float min_depth, float max_depth)
{
	const uint32_t viewport[] = { float_bits(width / 2.0f), float_bits(x + width / 2.0f), float_bits(-height / 2.0f),
		float_bits(y + height / 2.0f), float_bits(max_depth - min_depth), float_bits(min_depth) };
	GpuPackets packets = gpu_reserve_packets(VIEWPORT_DWORDS);
	gpu_put_registers(&packets, PA_CL_VPORT_XSCALE, viewport, 6);
	gpu_packets_done(&packets);
}

// Xe_pDrawIndexedPrimitive (xe.c:191-200).
void xsdk_gpu_draw_indexed(uint32_t primitive_type, uint32_t index_address, uint32_t index_count, bool indices_32_bit)
{
	assert(index_count && index_count <= MAX_INDICES && (index_address & (indices_32_bit ? 3u : 1u)) == 0);
	assert(!target_undone || xsdk_gpu_recording());
	GpuPackets packets = gpu_reserve_packets(DRAW_INDEXED_DWORDS);
	gpu_put_type3(&packets, PM4_DRAW_INDX, 4);
	gpu_put(&packets, 0);
	gpu_put(
		&packets, primitive_type | index_count << VERTEX_COUNT_SHIFT | (uint32_t)indices_32_bit << INDEX_32_BIT_SHIFT);
	gpu_put(&packets, index_address);
	gpu_put(&packets, (index_count | DMA_SWAP_8IN16) << indices_32_bit);
	gpu_packets_done(&packets);
}
