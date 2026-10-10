// The texture layout and fetch constant below come from libxenon:drivers/xenos/xe.c, rewritten for the SDK.
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

#include "xsdk/texture.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "gpu_private.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	TEXEL_BYTES_LOG2 = 2,
	// Linear rows start every 256 bytes (xenia:src/xenia/gpu/xenos.h:1179-1181).
	LINEAR_PITCH_ALIGNMENT = 256 >> TEXEL_BYTES_LOG2,
	// Tiled textures are whole 32 by 32 tiles, and libxenon pads linear ones to 32 rows too (xe.c:2082-2083).
	TILE_TEXELS = 32,
	// The fetch constant's fields (xenia:src/xenia/gpu/xenos.h:1184-1285), set as libxenon sets them (xe.c:1187-1207).
	FETCH_TYPE_TEXTURE = 2,
	CLAMP_TO_EDGE = 2,
	CLAMP_X_SHIFT = 10,
	CLAMP_Y_SHIFT = 13,
	CLAMP_Z_SHIFT = 16,
	PITCH_SHIFT = 22,
	PITCH_UNIT_LOG2 = 5,
	TILED_SHIFT = 31,
	FORMAT_8_8_8_8 = 6,
	ENDIAN_8IN32 = 2 << 6,
	HEIGHT_SHIFT = 13,
	// Red from z, green from y, blue from x and alpha from w, for words written as ARGB (xe.c:1202).
	SWIZZLE_ARGB = (2 | 1 << 3 | 0 << 6 | 3 << 9) << 1,
	// Point magnification and minification; libxenon's linear mip filter, with only the base level.
	MIP_FILTER_LINEAR = 1 << 23,
	DIMENSION_2D = 1 << 9,
	PACKED_MIPS = 1 << 11,
	FETCH_ADDRESS_ALIGNMENT = 0x1000,
};

static NTSTATUS reject(const char *why)
{
	xsdk_log_write("texture: %s", why);
	return STATUS_INVALID_PARAMETER;
}

static uint32_t round_up(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

// Xe_CreateTexture and TEXTURE_FETCH for one ARGB level, clamped and point-sampled.
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t width, uint32_t height, bool tiled)
{
	if (!width || !height || width > XSDK_TEXTURE_MAX_SIZE || height > XSDK_TEXTURE_MAX_SIZE)
		return reject("texture size not from 1 to 8192");
	const uint32_t pitch = round_up(width, tiled ? TILE_TEXELS : LINEAR_PITCH_ALIGNMENT);
	const uint32_t size_bytes = pitch * round_up(height, TILE_TEXELS) << TEXEL_BYTES_LOG2;
	volatile uint32_t *const texels = xsdk_gpu_alloc(size_bytes);
	if (!texels)
		return STATUS_NO_MEMORY;
	const uint32_t address = xsdk_gpu_address(texels);
	assert(address % FETCH_ADDRESS_ALIGNMENT == 0);
	*texture = (XsdkTexture){
		.texels = texels,
		.address = address,
		.size_bytes = size_bytes,
		.width = width,
		.height = height,
		.pitch = pitch,
		.tiled = tiled,
		.fetch = {
			FETCH_TYPE_TEXTURE | CLAMP_TO_EDGE << CLAMP_X_SHIFT | CLAMP_TO_EDGE << CLAMP_Y_SHIFT |
				CLAMP_TO_EDGE << CLAMP_Z_SHIFT | (pitch >> PITCH_UNIT_LOG2) << PITCH_SHIFT |
				(uint32_t)tiled << TILED_SHIFT,
			address | ENDIAN_8IN32 | FORMAT_8_8_8_8,
			(height - 1) << HEIGHT_SHIFT | (width - 1),
			MIP_FILTER_LINEAR | SWIZZLE_ARGB,
			0,
			PACKED_MIPS | DIMENSION_2D,
		},
	};
	return STATUS_SUCCESS;
}

void xsdk_texture_upload(XsdkTexture *texture, const uint32_t *argb, uint32_t row_pixels)
{
	for (uint32_t y = 0; y < texture->height; y++) {
		for (uint32_t x = 0; x < texture->width; x++) {
			const uint32_t index = texture->tiled
				? xsdk_tiled_2d(x, y, texture->pitch, TEXEL_BYTES_LOG2) >> TEXEL_BYTES_LOG2
				: y * texture->pitch + x;
			texture->texels[index] = argb[y * row_pixels + x];
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

void xsdk_texture_free(XsdkTexture *texture)
{
	xsdk_gpu_free(texture->texels);
	*texture = (XsdkTexture){ .texels = NULL };
}
