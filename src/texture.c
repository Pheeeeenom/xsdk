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
	PAGE_BYTES = 0x1000,
	// The fetch constant's fields (xenia:src/xenia/gpu/xenos.h:1184-1285), set as libxenon sets them (xe.c:1187-1207).
	FETCH_TYPE_TEXTURE = 2,
	CLAMP_X_SHIFT = 10,
	CLAMP_Y_SHIFT = 13,
	PITCH_SHIFT = 22,
	PITCH_UNIT_LOG2 = 5,
	TILED_SHIFT = 31,
	FORMAT_8_8_8_8 = 6,
	ENDIAN_8IN32 = 2 << 6,
	HEIGHT_SHIFT = 13,
	// Red from z, green from y, blue from x and alpha from w, for words written as ARGB (xe.c:1202).
	SWIZZLE_ARGB = (2 | 1 << 3 | 0 << 6 | 3 << 9) << 1,
	MAG_FILTER_SHIFT = 19,
	MIN_FILTER_SHIFT = 21,
	MIP_FILTER_SHIFT = 23,
	MIP_MAX_LEVEL_SHIFT = 6,
	LOD_BIAS_SHIFT = 12,
	// A signed 10-bit field with 5 fractional bits.
	LOD_BIAS_STEPS = 32,
	LOD_BIAS_MASK = 0x3FF,
	DIMENSION_2D = 1 << 9,
	// Levels at 16 texels and under share a tail (xenia:src/xenia/gpu/texture_util.h:76-83).
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

// Down to 1x1 on the long side, as Xenia counts them (xenia:src/xenia/gpu/texture_util.cc:68-74).
static uint32_t level_count(uint32_t width, uint32_t height)
{
	const uint32_t longest = width > height ? width : height;
	return 32 - (uint32_t)__builtin_clz(longest);
}

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

// Xe_CreateTexture and TEXTURE_FETCH for ARGB levels, the base then the mips, a page apart.
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t width, uint32_t height, uint32_t levels, bool tiled)
{
	if (!width || !height || width > XSDK_TEXTURE_MAX_SIZE || height > XSDK_TEXTURE_MAX_SIZE)
		return reject("texture size not from 1 to 8192");
	if (!levels || levels > level_count(width, height))
		return reject("levels not from 1 to a 1x1 level");
	if (!tiled && levels > 1) {
		xsdk_log_write("texture: linear textures have one level");
		return STATUS_NOT_IMPLEMENTED;
	}
	const uint32_t pitch = round_up(width, tiled ? TILE_TEXELS : LINEAR_PITCH_ALIGNMENT);
	const uint32_t mip_offset_bytes = round_up(pitch * round_up(height, TILE_TEXELS) << TEXEL_BYTES_LOG2, PAGE_BYTES);
	const uint32_t mip_bytes = xsdk_texture_mip_bytes(width, height, levels);
	const uint32_t size_bytes = mip_offset_bytes + mip_bytes;
	volatile uint32_t *const texels = xsdk_gpu_alloc(size_bytes);
	if (!texels)
		return STATUS_NO_MEMORY;
	const uint32_t address = xsdk_gpu_address(texels);
	assert(address % FETCH_ADDRESS_ALIGNMENT == 0);
	// Mips in the base's tail point back at the base, so Xenia reads them where the console does.
	const uint32_t mip_address = levels == 1 ? 0 : mip_bytes ? address + mip_offset_bytes : address;
	*texture = (XsdkTexture){
		.texels = texels,
		.address = address,
		.size_bytes = size_bytes,
		.width = width,
		.height = height,
		.levels = levels,
		.pitch = pitch,
		.tiled = tiled,
		.mip_offset_bytes = mip_offset_bytes,
		.fetch = {
			FETCH_TYPE_TEXTURE | (pitch >> PITCH_UNIT_LOG2) << PITCH_SHIFT | (uint32_t)tiled << TILED_SHIFT,
			address | ENDIAN_8IN32 | FORMAT_8_8_8_8,
			(height - 1) << HEIGHT_SHIFT | (width - 1),
			SWIZZLE_ARGB,
			(levels - 1) << MIP_MAX_LEVEL_SHIFT,
			mip_address | PACKED_MIPS | DIMENSION_2D,
		},
	};
	return STATUS_SUCCESS;
}

void xsdk_texture_upload(XsdkTexture *texture, uint32_t level, const uint32_t *argb, uint32_t row_pixels)
{
	assert(level < texture->levels);
	TextureLevelPlace place;
	xsdk_texture_place(texture->width, texture->height, texture->pitch, texture->mip_offset_bytes, level, &place);
	volatile uint32_t *const image = texture->texels + (place.offset_bytes >> TEXEL_BYTES_LOG2);
	for (uint32_t y = 0; y < level_size(texture->height, level); y++) {
		for (uint32_t x = 0; x < level_size(texture->width, level); x++) {
			const uint32_t stored_x = place.x + x;
			const uint32_t stored_y = place.y + y;
			const uint32_t index = texture->tiled
				? xsdk_tiled_2d(stored_x, stored_y, place.pitch, TEXEL_BYTES_LOG2) >> TEXEL_BYTES_LOG2
				: stored_y * place.pitch + stored_x;
			image[index] = argb[y * row_pixels + x];
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

void xsdk_texture_free(XsdkTexture *texture)
{
	xsdk_gpu_free(texture->texels);
	*texture = (XsdkTexture){ .texels = NULL };
}

void xsdk_texture_sampler_state(XsdkSamplerState *state, const XsdkSampler *sampler)
{
	assert(sampler->mag_filter <= XSDK_FILTER_LINEAR && sampler->min_filter <= XSDK_FILTER_LINEAR);
	assert(sampler->mip_filter <= XSDK_FILTER_LINEAR);
	assert(sampler->address_u <= XSDK_ADDRESS_CLAMP && sampler->address_v <= XSDK_ADDRESS_CLAMP);
	assert(sampler->lod_bias >= -16.0f && sampler->lod_bias < 16.0f);
	const float steps = sampler->lod_bias * LOD_BIAS_STEPS;
	const int32_t bias = (int32_t)(steps < 0.0f ? steps - 0.5f : steps + 0.5f);
	*state = (XsdkSamplerState){ .fetch = {
									 sampler->address_u << CLAMP_X_SHIFT | sampler->address_v << CLAMP_Y_SHIFT,
									 0,
									 0,
									 sampler->mag_filter << MAG_FILTER_SHIFT | sampler->min_filter << MIN_FILTER_SHIFT |
										 sampler->mip_filter << MIP_FILTER_SHIFT,
									 ((uint32_t)bias & LOD_BIAS_MASK) << LOD_BIAS_SHIFT,
									 0,
								 } };
}
