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

enum {
	DRAW_MAX_DWORDS = 96,
	// eDRAM tiles without multisampling (xenia:src/xenia/gpu/xenos.h:414-416).
	TILE_WIDTH = 80,
	TILE_HEIGHT = 16,
	EDRAM_TILES = 2048,
	EDRAM_MODE_COLOR_DEPTH = 4,
	// libxenon's control state from RB_DEPTHCONTROL on, less depth and culling (xe.c:1403-1416).
	BLEND_ONE_ZERO = 0x00010001,
	COLOR_CONTROL = 0x87000007,
	CLIP_D3D_SPACE = 0x00080000,
	SC_MODE_WINDOW_OFFSET_NO_CULL = 0x00010000,
	VTE_VIEWPORT_AND_W0 = 0x0000043F,
	STENCIL_MASKS = 0x00FFFF00,
	INTERPOLATE_ALL_AT_CENTERS = 0xFFFFFFFF,
	MAX_VERTEX_INDEX = 0x00FFFFFF,
	// Stream 0's vertex fetch constant and libxenon's fetch word: type 3, k8in32, bit 28 (xe.c:1181-1185, 1965).
	VERTEX_FETCH_CONSTANT = 95,
	VERTEX_FETCH_TYPE = 3,
	VERTEX_FETCH_8IN32 = 1 << 28 | 2,
	VERTEX_FETCH_SIZE_SHIFT = 2,
	AUTO_INDEX = 2 << 6,
	VERTEX_COUNT_SHIFT = 16,
};

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// Xe_SetRenderTarget's layout: depth then colour, as many tiles each, an even number across (xe.c:998-1036).
void xsdk_gpu_put_surface(GpuPackets *packets, uint32_t width, uint32_t height)
{
	const uint32_t tiles_across = ((width + TILE_WIDTH - 1) / TILE_WIDTH + 1) & ~1u;
	const uint32_t target_tiles = tiles_across * ((height + TILE_HEIGHT - 1) / TILE_HEIGHT);
	assert(2 * target_tiles <= EDRAM_TILES);
	const uint32_t edram_pitch = tiles_across * TILE_WIDTH;
	const uint32_t surface[] = { edram_pitch | edram_pitch << 18, target_tiles, 0 };
	gpu_put_registers(packets, RB_SURFACE_INFO, surface, 3);
}

// Xe_pSetState's control and miscellaneous state (xe.c:1688-1744).
void xsdk_gpu_set_target(uint32_t width, uint32_t height)
{
	const uint32_t control[] = { 0, BLEND_ONE_ZERO, COLOR_CONTROL, 0, CLIP_D3D_SPACE, SC_MODE_WINDOW_OFFSET_NO_CULL,
		VTE_VIEWPORT_AND_W0, 0, EDRAM_MODE_COLOR_DEPTH };
	const uint32_t indices[] = { MAX_VERTEX_INDEX, 0 };
	// The stencil masks, alpha reference 0, then a viewport over the target with z from 0 to 1.
	const uint32_t viewport[] = { STENCIL_MASKS, STENCIL_MASKS, 0, float_bits(width / 2.0f), float_bits(width / 2.0f),
		float_bits(-(float)height / 2.0f), float_bits(height / 2.0f), float_bits(1.0f), 0 };
	// Xe_pStuff's zeros before every draw (xe.c:1233-1238).
	const uint32_t polygon_offset[8] = { 0 };
	uint32_t dwords[DRAW_MAX_DWORDS];
	GpuPackets packets = { .dwords = dwords, .count = 0, .capacity = DRAW_MAX_DWORDS };
	gpu_put_registers(&packets, PA_SU_POLY_OFFSET_FRONT_SCALE, polygon_offset, 8);
	gpu_put_registers(&packets, RB_DEPTHCONTROL, control, 9);
	xsdk_gpu_put_window(&packets, width, height);
	xsdk_gpu_put_surface(&packets, width, height);
	gpu_put_register(&packets, COHER_DEST_BASE_7, 0);
	gpu_put_registers(&packets, VGT_MAX_VTX_INDX, indices, 2);
	gpu_put_register(&packets, RB_COLOR_MASK, 0xF);
	gpu_put_registers(&packets, RB_STENCILREFMASK_BF, viewport, 9);
	gpu_put_register(&packets, PA_SC_AA_CONFIG, 0);
	gpu_put_register(&packets, PA_SC_AA_MASK, 0xFFFF);
	xsdk_gpu_write(&packets);
}

// IM_LOAD takes address | type, then the size in dwords (xenia:src/xenia/gpu/command_processor.cc:1535-1539).
static void put_im_load(GpuPackets *packets, const XsdkShader *shader)
{
	gpu_put_type3(packets, PM4_IM_LOAD, 2);
	gpu_put(packets, shader->address | shader->type);
	gpu_put(packets, shader->dword_count);
}

// Xe_pUploadShader: the pixel shader first, then the vertex shader and the program registers (xe.c:1378-1401).
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel)
{
	assert(vertex->type == XSDK_SHADER_VERTEX && pixel->type == XSDK_SHADER_PIXEL);
	const uint32_t program[] = { vertex->program_control | pixel->program_control,
		vertex->context_misc | pixel->context_misc, INTERPOLATE_ALL_AT_CENTERS };
	uint32_t dwords[DRAW_MAX_DWORDS];
	GpuPackets packets = { .dwords = dwords, .count = 0, .capacity = DRAW_MAX_DWORDS };
	put_im_load(&packets, pixel);
	put_im_load(&packets, vertex);
	gpu_put_registers(&packets, SQ_PROGRAM_CNTL, program, 3);
	xsdk_gpu_write(&packets);
}

void xsdk_gpu_set_vertices(uint32_t address, uint32_t size_bytes)
{
	const uint32_t fetch[] = { address | VERTEX_FETCH_TYPE,
		VERTEX_FETCH_8IN32 | size_bytes / 4 << VERTEX_FETCH_SIZE_SHIFT };
	const uint32_t flush_fetch[] = { 0, 0, 0 };
	uint32_t dwords[DRAW_MAX_DWORDS];
	GpuPackets packets = { .dwords = dwords, .count = 0, .capacity = DRAW_MAX_DWORDS };
	// libxenon invalidates vertices the CPU wrote and flushes the fetch constants it set (xe.c:759, 1146).
	xsdk_gpu_put_coherency(&packets, COHER_VERTEX_CACHE, address, size_bytes);
	gpu_put_registers(&packets, SHADER_CONSTANT_FETCH_00_0 + VERTEX_FETCH_CONSTANT * 2, fetch, 2);
	gpu_put_registers(&packets, SHADER_CONSTANT_FLUSH_FETCH_0, flush_fetch, 3);
	xsdk_gpu_write(&packets);
}

// Xe_DrawPrimitive, with DRAW_INDX_2 as the resolve uses it (xe.c:1943-1957).
void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count)
{
	uint32_t dwords[DRAW_MAX_DWORDS];
	GpuPackets packets = { .dwords = dwords, .count = 0, .capacity = DRAW_MAX_DWORDS };
	gpu_put_register(&packets, COHER_DEST_BASE_1, 0);
	gpu_put_register(&packets, VGT_INDX_OFFSET, 0);
	gpu_put_type3(&packets, PM4_DRAW_INDX_2, 1);
	gpu_put(&packets, primitive_type | AUTO_INDEX | vertex_count << VERTEX_COUNT_SHIFT);
	xsdk_gpu_write(&packets);
}
