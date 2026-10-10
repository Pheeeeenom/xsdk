// The sequences below come from libxenon:drivers/xenos/xe.c, rewritten with Xenia's register names.
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

#ifndef XSDK_LIBXENON_GPU_H
#define XSDK_LIBXENON_GPU_H

#include <stdint.h>

#include "probe_gpu.h"

enum {
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	INVALIDATE_VERTEX_SHADER = 1 << 8,
	INVALIDATE_SHADERS = 3 << 8,
	SHADER_TYPE_VERTEX = 0,
	SHADER_TYPE_PIXEL = 1,
	// COHER_STATUS_HOST's vc_action_ena (xenia:src/xenia/gpu/registers.h:84).
	COHER_VERTEX_CACHE = 1 << 24,
	COHER_RESOLVE_ACTIONS = 0x03000100,
	COHER_BUSY = 0x80000000,
	WAIT_REG_MEM_EQUAL = 3,
	WAIT_REG_MEM_INTERVAL = 8,
	CACHE_FLUSH = 6,
	ALL_MEMORY_BYTES = 0x20000000,
	POINT_DRAWS = 24,
	// A one-point list, auto-indexed, and a three-vertex rectangle list (xenia:src/xenia/gpu/registers.h:311).
	POINT_LIST_AUTO_INDEX_1 = 1 | 2 << 6 | 1 << 16,
	RECTANGLE_LIST_AUTO_INDEX_3 = 8 | 2 << 6 | 3 << 16,
	CLIP_DISABLE = 1 << 16,
	VTX_WINDOW_OFFSET_ENABLE = 1 << 16,
	VTE_XY_Z_ALREADY_TRANSFORMED = 0x300,
	PIX_CENTER_D3D = 4,
	EDRAM_MODE_COLOR_DEPTH = 4,
	EDRAM_MODE_COPY = 6,
	// A 640x480 target's eDRAM layout: pitch 640, colour after 240 tiles of depth (xe.c:998-1028, 1030-1036).
	SURFACE_INFO_640 = 640 | 640 << 18,
	COLOR_INFO_BASE_240 = 240,
	// Copy colour, clear colour and depth, k_8_8_8_8 with the swap bit (xe.c:1104-1108).
	COPY_CONTROL_CLEAR_COLOR_DEPTH = 1 << 20 | 3 << 8,
	COPY_DEST_INFO_8888_SWAP = 1 << 24 | 6 << 7,
	DEPTH_CLEAR_FAR = 0xFFFFFF00,
	DEPTH_STENCIL_ALWAYS_WRITE = 0x8777,
	PROGRAM_CNTL_RESOLVE = 2 | 1 << 16,
	VERTEX_FETCH_TYPE = 3,
	VERTEX_FETCH_SIZE_6_8IN32 = 1 << 28 | 6 << 2 | 2,
};

// Xe_pSetSurfaceClip with no offset (xe.c:157-163).
static void put_libxenon_window(ProbeStage *stage, uint32_t width, uint32_t height)
{
	const uint32_t window[] = { 0, 0, width | height << 16 };
	probe_put_registers(stage, PA_SC_WINDOW_OFFSET, window, 3);
}

// Xe_pInvalidateGpuCache and Xe_pInvalidateGpuCacheAll (xe.c:732-750).
static void put_libxenon_coherency(ProbeStage *stage, uint32_t actions, uint32_t base, uint32_t size_bytes)
{
	const uint32_t range[] = { size_bytes, base };
	probe_put_register(stage, COHER_STATUS_HOST, actions);
	probe_put_registers(stage, COHER_SIZE_HOST, range, 2);
	probe_put_type3(stage, PM4_WAIT_REG_MEM, 5);
	probe_put(stage, WAIT_REG_MEM_EQUAL);
	probe_put(stage, COHER_STATUS_HOST);
	probe_put(stage, 0);
	probe_put(stage, COHER_BUSY);
	probe_put(stage, WAIT_REG_MEM_INTERVAL);
}

// The ME_INIT that starts Xe_pGInit (xe.c:556-561).
static void put_libxenon_me_init(ProbeStage *stage)
{
	static const uint32_t payload[] = {
		0x000003FF,
		0,
		0,
		0,
		0x80,
		0x100,
		0x180,
		0x200,
		0x280,
		0x300,
		0x380,
		0x00010800,
		7,
		0,
		0,
		0,
		0,
		0,
	};
	probe_put_type3(stage, PM4_ME_INIT, sizeof payload / 4);
	for (uint32_t i = 0; i < sizeof payload / 4; i++)
		probe_put(stage, payload[i]);
}

// Xe_pGInit4 and Xe_pGInit5, less what the probe only logs or leaves out (xe.c:464-498).
static void put_libxenon_shader_unit(ProbeStage *stage)
{
	probe_put_register(stage, SQ_INST_STORE_MANAGMENT, 0x00010800);
	probe_put_register(stage, WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	probe_put_register(stage, SQ_EO_RT, 0);
	probe_put_register(stage, SQ_FLOW_CONTROL, 0x04000000);
	probe_put_type3(stage, PM4_REG_RMW, 3);
	probe_put(stage, UNKNOWN_0081);
	probe_put(stage, 0xFFFFFFFF);
	probe_put(stage, 0x80010000);
	probe_put_type3(stage, PM4_REG_RMW, 3);
	probe_put(stage, UNKNOWN_0082);
	probe_put(stage, 0xFFFFFFFF);
	probe_put(stage, 0);
	probe_put_register(stage, UNKNOWN_0E42, 0x1F60);
	probe_put_register(stage, PA_CL_ENHANCE, 3);
}

// Xe_pGInit6, Xe_pGInit10 and Xe_pGInit7, less libxenon's scratch writes (xe.c:500-515, 540-552).
static void put_libxenon_render_backend(ProbeStage *stage)
{
	put_libxenon_window(stage, 1024, 720);
	put_libxenon_window(stage, 1024, 720);
	probe_put_type3(stage, PM4_INVALIDATE_STATE, 1);
	probe_put(stage, INVALIDATE_SHADERS);
	probe_put_register(stage, WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	probe_put_register(stage, RB_BC_CONTROL, 0x0000200E);
}

static void put_libxenon_shader(ProbeStage *stage, uint32_t type, const uint32_t *shader, uint32_t dword_count)
{
	probe_put_type3(stage, PM4_IM_LOAD_IMMEDIATE, 2 + dword_count);
	probe_put(stage, type);
	probe_put(stage, dword_count);
	for (uint32_t i = 0; i < dword_count; i++)
		probe_put(stage, shader[i]);
}

// Xe_pGInit0: a vertex and a pixel shader, and draw state (xe.c:398-441).
static void put_libxenon_draw_state(ProbeStage *stage)
{
	static const uint32_t vertex_shader[] = {
		0x00001003,
		0x00001200,
		0xC4000000,
		0x00001004,
		0x00001200,
		0xC2000000,
		0x00001005,
		0x10061200,
		0x22000000,
		0xC8000000,
		0x00000000,
		0x02000000,
		0xC800C000,
		0x00000000,
		0xC2000000,
		0xC888C03E,
		0x00000000,
		0xC2010100,
		0xC8000000,
		0x00000000,
		0x02000000,
		0x00000000,
		0x00000000,
		0x00000000,
	};
	static const uint32_t pixel_shader[] = {
		0x00000000,
		0x1001C400,
		0x22000000,
		0xC80F8000,
		0x00000000,
		0xC2000000,
		0x00000000,
		0x00000000,
		0x00000000,
	};
	const uint32_t program[] = { 0x1000000E, 0 };
	const uint32_t indices[] = { 0xFFFF, 0, 0 };
	const uint32_t clip[] = { CLIP_DISABLE, VTX_WINDOW_OFFSET_ENABLE, VTE_XY_Z_ALREADY_TRANSFORMED };
	probe_put_type3(stage, PM4_INVALIDATE_STATE, 1);
	probe_put(stage, INVALIDATE_SHADERS);
	put_libxenon_shader(stage, SHADER_TYPE_VERTEX, vertex_shader, sizeof vertex_shader / 4);
	put_libxenon_shader(stage, SHADER_TYPE_PIXEL, pixel_shader, sizeof pixel_shader / 4);
	probe_put_registers(stage, SQ_PROGRAM_CNTL, program, 2);
	probe_put_registers(stage, VGT_MAX_VTX_INDX, indices, 3);
	probe_put_registers(stage, PA_CL_CLIP_CNTL, clip, 3);
	probe_put_register(stage, PA_SC_AA_MASK, 0xFFFF);
	probe_put_register(stage, COHER_DEST_BASE_7, 0);
	probe_put_register(stage, RB_DEPTHCONTROL, 0);
	probe_put_register(stage, RB_HIZCONTROL, 0);
	probe_put_register(stage, RB_MODECONTROL, EDRAM_MODE_COLOR_DEPTH);
	probe_put_register(stage, RB_COLOR_MASK, 0);
	probe_put_register(stage, PA_SU_POINT_SIZE, 0x00080008);
	probe_put_register(stage, PA_SU_VTX_CNTL, PIX_CENTER_D3D);
	put_libxenon_window(stage, 16, 16);
}

// Xe_pGInit1 for 0x10 to 0x70, stepping the shader unit's register split (xe.c:443-452, 527-528).
static void put_libxenon_register_split(ProbeStage *stage)
{
	for (uint32_t i = 0x10; i <= 0x70; i++) {
		const uint32_t split = i << 12 | (0x80 - i) << 4;
		probe_put_register(stage, WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
		probe_put_register_repeated(stage, SQ_GPR_MANAGEMENT, split | 1, 8);
		probe_put_register(stage, SQ_GPR_MANAGEMENT, split);
	}
}

// Xe_pGInit2 and Xe_pGInit8: 24 one-point draws, then the window again (xe.c:454-462, 517-520).
static void put_libxenon_point_draws(ProbeStage *stage)
{
	for (uint32_t i = 0; i < POINT_DRAWS; i++) {
		probe_put_type3(stage, PM4_DRAW_INDX_2, 1);
		probe_put(stage, POINT_LIST_AUTO_INDEX_1);
	}
	put_libxenon_window(stage, 1024, 720);
}

// Xe_ResolveInto for a 640x480 target, less its scratch write: copy eDRAM out, then clear it (xe.c:1069-1165).
static void put_libxenon_resolve(ProbeStage *stage, uint32_t clear, uint32_t destination, uint32_t vertices,
	const uint32_t *shader, uint32_t shader_dwords)
{
	const uint32_t width = 640;
	const uint32_t height = 480;
	const uint32_t surface[] = { SURFACE_INFO_640, COLOR_INFO_BASE_240, 0 };
	const uint32_t viewport[] = { 0x44000000, 0x44000000, 0xC3B40000, 0x43B40000, 0x3F800000, 0 };
	const uint32_t copy[] = { COPY_CONTROL_CLEAR_COLOR_DEPTH, destination, width | height << 16,
		COPY_DEST_INFO_8888_SWAP };
	const uint32_t program[] = { PROGRAM_CNTL_RESOLVE, 0 };
	const uint32_t indices[] = { 0xFFFF, 0, 0 };
	const uint32_t clip[] = { CLIP_DISABLE, VTX_WINDOW_OFFSET_ENABLE, VTE_XY_Z_ALREADY_TRANSFORMED };
	const uint32_t fetch[] = { vertices | VERTEX_FETCH_TYPE, VERTEX_FETCH_SIZE_6_8IN32, 0, 0, 0, 0 };
	const uint32_t flush_fetch[] = { 0, 0, 0 };
	put_libxenon_window(stage, width, height);
	// Xe_VBEnd invalidates the vertex cache over the vertices' pool: 12000 bytes and a page (xe.c:1863-1871).
	put_libxenon_coherency(stage, COHER_VERTEX_CACHE, vertices, 12000 + 0x1000);
	probe_put_registers(stage, RB_SURFACE_INFO, surface, 3);
	probe_put_register(stage, RB_COLOR_MASK, 0xF);
	probe_put_registers(stage, PA_CL_VPORT_XSCALE, viewport, 6);
	probe_put_registers(stage, RB_COPY_CONTROL, copy, 4);
	probe_put_register(stage, RB_DEPTH_CLEAR, DEPTH_CLEAR_FAR);
	probe_put_register(stage, RB_COLOR_CLEAR, clear);
	probe_put_register(stage, RB_COLOR_CLEAR_LO, clear);
	probe_put_register(stage, RB_MODECONTROL, EDRAM_MODE_COPY);
	probe_put_register(stage, RB_MODECONTROL, EDRAM_MODE_COPY);
	probe_put_register(stage, RB_DEPTHCONTROL, DEPTH_STENCIL_ALWAYS_WRITE);
	probe_put_register(stage, WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	probe_put_type3(stage, PM4_INVALIDATE_STATE, 1);
	probe_put(stage, INVALIDATE_VERTEX_SHADER);
	// libxenon loads its 12-dword shader as 15, with three zeros after it.
	probe_put_type3(stage, PM4_IM_LOAD_IMMEDIATE, 2 + shader_dwords + 3);
	probe_put(stage, SHADER_TYPE_VERTEX);
	probe_put(stage, shader_dwords + 3);
	for (uint32_t i = 0; i < shader_dwords + 3; i++)
		probe_put(stage, i < shader_dwords ? shader[i] : 0);
	probe_put_registers(stage, SQ_PROGRAM_CNTL, program, 2);
	probe_put_register(stage, RB_HIZCONTROL, 0);
	probe_put_registers(stage, VGT_MAX_VTX_INDX, indices, 3);
	probe_put_registers(stage, PA_CL_CLIP_CNTL, clip, 3);
	probe_put_register(stage, PA_SC_AA_MASK, 0xFFFF);
	probe_put_register(stage, COHER_DEST_BASE_7, 0);
	probe_put_registers(stage, SHADER_CONSTANT_FETCH_00_0, fetch, 6);
	probe_put_registers(stage, SHADER_CONSTANT_FLUSH_FETCH_0, flush_fetch, 3);
	probe_put_type3(stage, PM4_DRAW_INDX_2, 1);
	probe_put(stage, RECTANGLE_LIST_AUTO_INDEX_3);
	probe_put_type3(stage, PM4_EVENT_WRITE, 1);
	probe_put(stage, CACHE_FLUSH);
	probe_put_register(stage, COHER_DEST_BASE_1, 0);
	put_libxenon_coherency(stage, COHER_RESOLVE_ACTIONS, destination, width * height * 4);
	probe_put_register(stage, RB_COPY_CONTROL, 0);
	probe_put_register(stage, RB_COPY_DEST_INFO, 0);
	// Xe_Sync's wait for idle (xe.c:1223-1226, 1278-1282).
	probe_put_register(stage, WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
}

#endif
