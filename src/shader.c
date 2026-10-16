// The loader and patcher below come from libxenon:drivers/xenos/xe.c and xe.h, rewritten for the SDK.
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

#include "xsdk/shader.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu_private.h"
#include "microcode_private.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Container dwords: the header's magic, microcode base and record offset (xe.h:111-121, xe.c:614-629).
enum {
	HEADER_MAGIC = 0,
	HEADER_MICROCODE_BASE = 1,
	HEADER_RECORD_OFFSET = 6,
	HEADER_DWORDS = 9,
	MAGIC_MASK = 0xFFFFFF00,
	MAGIC = 0x102A1100,
	MAGIC_VERTEX = 1,
};

// Shader record dwords, then a vertex shader's skip count, fetch count and patch table (xe.h:123-129, xe.c:808-814).
enum {
	RECORD_MICROCODE_OFFSET = 0,
	RECORD_MICROCODE_BYTES = 1,
	RECORD_PROGRAM_CONTROL = 2,
	RECORD_CONTEXT_MISC = 3,
	PIXEL_RECORD_DWORDS = 4,
	RECORD_SKIP = 6,
	RECORD_FETCH_COUNT = 7,
	VERTEX_RECORD_DWORDS = 8,
	SLOT_DWORDS = 3,
};

// Vertex fetch fields (xenia:src/xenia/gpu/ucode.h:745-777) and a patch record's (xe.c:821-823).
enum {
	FETCH_CONSTANT_INDEX_SHIFT = 20,
	FETCH_CONSTANT_SELECT_SHIFT = 25,
	FETCH_CONSTANT_MASK = 0x7F << FETCH_CONSTANT_INDEX_SHIFT,
	FETCH_PREFETCH_MASK = 0x38000000,
	FETCH_SWIZZLE_MASK = 0xFFF,
	FETCH_SIGNED = 1 << 12,
	FETCH_INTEGER = 1 << 13,
	// Clear for D3D's signed mapping, -1 clamped (xenia:src/xenia/gpu/xenos.h:761-767).
	FETCH_NO_ZERO = 1 << 14,
	FETCH_FORMAT_SHIFT = 16,
	FETCH_FORMAT_MASK = 0x3F << FETCH_FORMAT_SHIFT,
	FETCH_MINI = 1 << 30,
	FETCH_STRIDE_MASK = 0xFF,
	FETCH_OFFSET_SHIFT = 8,
	FETCH_OFFSET_MASK = 0x7FFFFF00,
	PATCH_SLOT_MASK = 0xFFF,
	PATCH_USAGE_SHIFT = 12,
	PATCH_INDEX_SHIFT = 16,
	PATCH_FIELD_MASK = 0xF,
	// Stream 0's vertex fetch constant, as the GPU layer binds it; vertex fetch constants go in threes.
	STREAM_0_FETCH_CONSTANT = 95,
	FETCHES_PER_GROUP = 3,
	// A fetch's destination swizzle: 3 bits a component, x to w, then 0 and 1 (xenia:src/xenia/gpu/ucode.h:677-688).
	SWIZZLE_BITS = 3,
	SWIZZLE_SOURCE_MASK = 7,
	SWIZZLE_W = 3,
	SWIZZLE_ZERO = 4,
	SWIZZLE_ONE = 5,
};

typedef struct {
	uint8_t format;
	uint8_t components;
	bool is_signed;
	bool normalized;
} VertexFormatInfo;

// VertexFormat values and component counts (xenia:src/xenia/gpu/xenos.h:602-646).
static const VertexFormatInfo vertex_formats[XSDK_VERTEX_FORMATS] = {
	[XSDK_VERTEX_FLOAT1] = { 36, 1, false, false },
	[XSDK_VERTEX_FLOAT2] = { 37, 2, false, false },
	[XSDK_VERTEX_FLOAT3] = { 57, 3, false, false },
	[XSDK_VERTEX_FLOAT4] = { 38, 4, false, false },
	[XSDK_VERTEX_HALF2] = { 31, 2, false, false },
	[XSDK_VERTEX_HALF4] = { 32, 4, false, false },
	[XSDK_VERTEX_UBYTE4N] = { 6, 4, false, true },
	[XSDK_VERTEX_BYTE4N] = { 6, 4, true, true },
	[XSDK_VERTEX_UBYTE4] = { 6, 4, false, false },
	[XSDK_VERTEX_BYTE4] = { 6, 4, true, false },
	[XSDK_VERTEX_USHORT2N] = { 25, 2, false, true },
	[XSDK_VERTEX_SHORT2N] = { 25, 2, true, true },
	[XSDK_VERTEX_USHORT2] = { 25, 2, false, false },
	[XSDK_VERTEX_SHORT2] = { 25, 2, true, false },
	[XSDK_VERTEX_USHORT4N] = { 26, 4, false, true },
	[XSDK_VERTEX_SHORT4N] = { 26, 4, true, true },
	[XSDK_VERTEX_USHORT4] = { 26, 4, false, false },
	[XSDK_VERTEX_SHORT4] = { 26, 4, true, false },
};

static NTSTATUS reject(const char *why)
{
	xsdk_log_write("shader: %s", why);
	return STATUS_INVALID_PARAMETER;
}

static const XsdkVertexElement *find_element(
	const XsdkVertexElement *elements, uint32_t element_count, uint32_t usage, uint32_t usage_index)
{
	for (uint32_t i = 0; i < element_count; i++) {
		if (elements[i].usage == usage && elements[i].usage_index == usage_index)
			return &elements[i];
	}
	return NULL;
}

// Xe_LoadShaderFromMemory and Xe_InstantiateShader (xe.c:613-629, 935-947).
NTSTATUS xsdk_shader_load(XsdkShader *shader, const uint32_t *container, uint32_t size_bytes)
{
	const uint32_t container_dwords = size_bytes / 4;
	if (container_dwords < HEADER_DWORDS || (container[HEADER_MAGIC] & MAGIC_MASK) != MAGIC)
		return reject("not a shader container");
	const bool is_vertex = (container[HEADER_MAGIC] & ~MAGIC_MASK) == MAGIC_VERTEX;
	const uint32_t record_dword = container[HEADER_RECORD_OFFSET] / 4;
	if (record_dword + (is_vertex ? VERTEX_RECORD_DWORDS : PIXEL_RECORD_DWORDS) > container_dwords)
		return reject("shader record out of bounds");
	const uint32_t *record = &container[record_dword];
	const uint32_t microcode_offset = container[HEADER_MICROCODE_BASE] + record[RECORD_MICROCODE_OFFSET];
	const uint32_t microcode_bytes = record[RECORD_MICROCODE_BYTES];
	if (microcode_offset % 4 || microcode_bytes % (SLOT_DWORDS * 4) || microcode_offset > size_bytes ||
		microcode_bytes > size_bytes - microcode_offset)
		return reject("microcode out of bounds");
	uint32_t fetch_dword = 0;
	uint32_t fetch_count = 0;
	if (is_vertex) {
		fetch_dword = record_dword + VERTEX_RECORD_DWORDS + record[RECORD_SKIP] * 2;
		fetch_count = record[RECORD_FETCH_COUNT];
		if (fetch_dword + fetch_count > container_dwords)
			return reject("fetch patch records out of bounds");
	}

	volatile uint32_t *const microcode = xsdk_microcode_alloc(microcode_bytes);
	if (!microcode)
		return STATUS_NO_MEMORY;
	const uint32_t dword_count = microcode_bytes / 4;
	for (uint32_t i = 0; i < dword_count; i++)
		microcode[i] = container[microcode_offset / 4 + i];
	__asm__ volatile("sync" ::: "memory");
	*shader = (XsdkShader){
		.microcode = microcode,
		.address = xsdk_gpu_address(microcode),
		.dword_count = dword_count,
		.type = is_vertex ? XSDK_SHADER_VERTEX : XSDK_SHADER_PIXEL,
		.program_control = record[RECORD_PROGRAM_CONTROL],
		.context_misc = record[RECORD_CONTEXT_MISC],
		.fetches = fetch_count ? &container[fetch_dword] : NULL,
		.fetch_count = fetch_count,
		.source = &container[microcode_offset / 4],
	};
	return STATUS_SUCCESS;
}

// The fetch's swizzle with each of x to w the component the element picks for it, as the swizzle chooses among the
// fetched components (xenia:src/xenia/gpu/ucode.h:677-688); components the data lacks read as 0, and w as 1, as
// libxenon pads them (xe.c:900-916).
static uint32_t picked_swizzle(uint32_t swizzle, const XsdkVertexElement *element, uint32_t components)
{
	for (uint32_t component = 0; component < 4; component++) {
		const uint32_t shift = component * SWIZZLE_BITS;
		uint32_t source = swizzle >> shift & SWIZZLE_SOURCE_MASK;
		if (source > SWIZZLE_W)
			continue;
		const uint32_t own = source == SWIZZLE_W ? SWIZZLE_ONE : SWIZZLE_ZERO;
		if (element->components[source])
			source = element->components[source] - XSDK_COMPONENT_X;
		swizzle = (swizzle & ~(SWIZZLE_SOURCE_MASK << shift)) | (source >= components ? own : source) << shift;
	}
	return swizzle;
}

static bool picks_in_range(const XsdkVertexElement *element)
{
	for (uint32_t i = 0; i < 4; i++) {
		if (element->components[i] > XSDK_COMPONENT_W)
			return false;
	}
	return true;
}

// Xe_ShaderApplyVFetchPatches for one-dword prefetches, with each element's stream and format (xe.c:788-933).
NTSTATUS xsdk_shader_set_vertex_layout(
	XsdkShader *shader, const XsdkVertexElement *elements, uint32_t element_count, const uint32_t *stream_strides)
{
	assert(shader->type == XSDK_SHADER_VERTEX);
	for (uint32_t i = 0; i < shader->fetch_count; i++) {
		const uint32_t patch = shader->fetches[i];
		const uint32_t slot = patch & PATCH_SLOT_MASK;
		const XsdkVertexElement *const element = find_element(elements, element_count,
			patch >> PATCH_USAGE_SHIFT & PATCH_FIELD_MASK, patch >> PATCH_INDEX_SHIFT & PATCH_FIELD_MASK);
		if (!element)
			return reject("the vertex layout lacks an input the shader fetches");
		if (element->format >= XSDK_VERTEX_FORMATS || element->stream >= XSDK_VERTEX_STREAMS ||
			!picks_in_range(element))
			return reject("vertex format, stream or component out of range");
		const uint32_t stride_bytes = stream_strides[element->stream];
		if (stride_bytes % 4 || stride_bytes / 4 > FETCH_STRIDE_MASK)
			return reject("vertex stride not a whole number of dwords up to 255");
		if (element->offset_bytes % 4 || (slot + 1) * SLOT_DWORDS > shader->dword_count)
			return reject("vertex element or fetch slot out of range");
		const VertexFormatInfo *const info = &vertex_formats[element->format];
		const uint32_t constant = STREAM_0_FETCH_CONSTANT - element->stream;
		const uint32_t *const source = &shader->source[slot * SLOT_DWORDS];
		volatile uint32_t *const fetch = &shader->microcode[slot * SLOT_DWORDS];
		fetch[0] = (source[0] & ~(FETCH_CONSTANT_MASK | FETCH_PREFETCH_MASK)) |
			constant / FETCHES_PER_GROUP << FETCH_CONSTANT_INDEX_SHIFT |
			constant % FETCHES_PER_GROUP << FETCH_CONSTANT_SELECT_SHIFT;
		fetch[1] =
			(source[1] &
				~(FETCH_SWIZZLE_MASK | FETCH_SIGNED | FETCH_INTEGER | FETCH_NO_ZERO | FETCH_FORMAT_MASK | FETCH_MINI)) |
			picked_swizzle(source[1] & FETCH_SWIZZLE_MASK, element, info->components) |
			(uint32_t)info->is_signed * FETCH_SIGNED | (uint32_t)!info->normalized * FETCH_INTEGER |
			(uint32_t)info->format << FETCH_FORMAT_SHIFT;
		fetch[2] = (source[2] & ~(FETCH_OFFSET_MASK | FETCH_STRIDE_MASK)) |
			element->offset_bytes / 4 << FETCH_OFFSET_SHIFT | stride_bytes / 4;
	}
	__asm__ volatile("sync" ::: "memory");
	// The GPU may hold the shader as it was.
	xsdk_gpu_forget_shaders();
	return STATUS_SUCCESS;
}

void xsdk_shader_free(XsdkShader *shader)
{
	// Another shader may come to sit where this one was.
	xsdk_gpu_forget_shaders();
	xsdk_microcode_free(shader->microcode, shader->dword_count * 4);
	*shader = (XsdkShader){ .microcode = NULL };
}
