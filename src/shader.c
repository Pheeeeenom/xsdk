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
	FETCH_PREFETCH_MASK = 0x38000000,
	FETCH_SIGNED = 1 << 12,
	FETCH_INTEGER = 1 << 13,
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

	volatile uint32_t *const microcode = xsdk_gpu_alloc(microcode_bytes);
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
	};
	return STATUS_SUCCESS;
}

// Xe_ShaderApplyVFetchPatches for one-dword prefetches and float4 elements (xe.c:788-933).
NTSTATUS xsdk_shader_set_vertex_layout(
	XsdkShader *shader, const XsdkVertexElement *elements, uint32_t element_count, uint32_t stride_bytes)
{
	assert(shader->type == XSDK_SHADER_VERTEX);
	if (stride_bytes % 4 || stride_bytes / 4 > FETCH_STRIDE_MASK)
		return reject("vertex stride not a whole number of dwords up to 255");
	for (uint32_t i = 0; i < shader->fetch_count; i++) {
		const uint32_t patch = shader->fetches[i];
		const uint32_t slot = patch & PATCH_SLOT_MASK;
		const XsdkVertexElement *const element = find_element(elements, element_count,
			patch >> PATCH_USAGE_SHIFT & PATCH_FIELD_MASK, patch >> PATCH_INDEX_SHIFT & PATCH_FIELD_MASK);
		if (!element)
			return reject("the vertex layout lacks an input the shader fetches");
		if (element->format != XSDK_VERTEX_FLOAT4)
			return STATUS_NOT_IMPLEMENTED;
		if (element->offset_bytes % 4 || (slot + 1) * SLOT_DWORDS > shader->dword_count)
			return reject("vertex element or fetch slot out of range");
		volatile uint32_t *const fetch = &shader->microcode[slot * SLOT_DWORDS];
		fetch[0] = fetch[0] & ~FETCH_PREFETCH_MASK;
		// libxenon marks every format but 8_8_8_8 as integer, and none as signed (xe.c:858-863).
		fetch[1] = (fetch[1] & ~(FETCH_FORMAT_MASK | FETCH_SIGNED | FETCH_INTEGER | FETCH_MINI)) |
			element->format << FETCH_FORMAT_SHIFT | FETCH_INTEGER;
		fetch[2] = (fetch[2] & ~(FETCH_OFFSET_MASK | FETCH_STRIDE_MASK)) |
			element->offset_bytes / 4 << FETCH_OFFSET_SHIFT | stride_bytes / 4;
	}
	__asm__ volatile("sync" ::: "memory");
	return STATUS_SUCCESS;
}

void xsdk_shader_free(XsdkShader *shader)
{
	xsdk_gpu_free(shader->microcode);
	*shader = (XsdkShader){ .microcode = NULL };
}
