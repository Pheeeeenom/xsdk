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
#include <string.h>

#include "gpu_private.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	// Linear rows start every 256 bytes (xenia:src/xenia/gpu/xenos.h:1179-1181).
	LINEAR_ROW_ALIGNMENT_BYTES = 256,
	// Tiled textures are whole 32 by 32 tiles of blocks, and libxenon pads linear ones to 32 rows too (xe.c:2082-2083).
	TILE_BLOCKS = 32,
	PAGE_BYTES = 0x1000,
	WORD_BYTES = 4,
	// The fetch constant's fields (xenia:src/xenia/gpu/xenos.h:1184-1285), set as libxenon sets them (xe.c:1187-1207).
	FETCH_TYPE_TEXTURE = 2,
	CLAMP_X_SHIFT = 10,
	CLAMP_Y_SHIFT = 13,
	PITCH_SHIFT = 22,
	PITCH_UNIT_LOG2 = 5,
	TILED_SHIFT = 31,
	ENDIAN_SHIFT = 6,
	HEIGHT_SHIFT = 13,
	SWIZZLE_SHIFT = 1,
	SWIZZLE_BITS = 3,
	SWIZZLE_MASK = 0xFFF,
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
	// In the tiled layout, a row's blocks sit together for 16 bytes, or 8 blocks of a byte: x's low 3 bits and the
	// offset's low 4 stay where they are (xsdk_tiled_2d).
	TILED_RUN_BYTES = 16,
	TILED_RUN_BLOCKS_MAX = 8,
};

// Endian swaps (xenia:src/xenia/gpu/xenos.h:193-198); libxenon marks 16-bit formats 8-in-16 (xe.h:184).
enum {
	ENDIAN_NONE = 0,
	ENDIAN_8IN16 = 1,
	ENDIAN_8IN32 = 2,
};

// Swizzle sources: the components x to w, then 0 and 1 (xenia:src/xenia/gpu/xenos.h:1015-1030).
enum {
	X,
	Y,
	Z,
	W,
	ZERO,
	ONE,
};

// Where each of red, green, blue and alpha comes from.
#define SWIZZLE(red, green, blue, alpha) ((red) | (green) << 3 | (blue) << 6 | (alpha) << 9)

typedef struct {
	uint32_t format;
	// Texels across and down a block, as a power of two.
	uint32_t block_log2;
	uint32_t block_bytes_log2;
	uint32_t endian;
	uint32_t swizzle;
} FormatInfo;

// TextureFormat values and block sizes (xenia:src/xenia/gpu/xenos.h:450-508, texture_info_formats.cc:25-55). Each
// format's x is its lowest bits or first byte, as Xenia maps them to Vulkan (vulkan/vulkan_texture_cache.cc:94-265).
static const FormatInfo formats[XSDK_FORMATS] = {
	[XSDK_FORMAT_A8R8G8B8] = { 6, 0, 2, ENDIAN_8IN32, SWIZZLE(Z, Y, X, W) },
	[XSDK_FORMAT_R5G6B5] = { 4, 0, 1, ENDIAN_8IN16, SWIZZLE(Z, Y, X, ONE) },
	[XSDK_FORMAT_A1R5G5B5] = { 3, 0, 1, ENDIAN_8IN16, SWIZZLE(Z, Y, X, W) },
	[XSDK_FORMAT_A4R4G4B4] = { 15, 0, 1, ENDIAN_8IN16, SWIZZLE(Z, Y, X, W) },
	[XSDK_FORMAT_A2B10G10R10] = { 7, 0, 2, ENDIAN_8IN32, SWIZZLE(X, Y, Z, W) },
	[XSDK_FORMAT_R8] = { 2, 0, 0, ENDIAN_NONE, SWIZZLE(X, ZERO, ZERO, ONE) },
	[XSDK_FORMAT_R8G8] = { 10, 0, 1, ENDIAN_NONE, SWIZZLE(X, Y, ZERO, ONE) },
	[XSDK_FORMAT_R16F] = { 30, 0, 1, ENDIAN_8IN16, SWIZZLE(X, ZERO, ZERO, ONE) },
	[XSDK_FORMAT_R16G16F] = { 31, 0, 2, ENDIAN_8IN16, SWIZZLE(X, Y, ZERO, ONE) },
	[XSDK_FORMAT_R16G16B16A16F] = { 32, 0, 3, ENDIAN_8IN16, SWIZZLE(X, Y, Z, W) },
	[XSDK_FORMAT_DXT1] = { 18, 2, 3, ENDIAN_8IN16, SWIZZLE(X, Y, Z, W) },
	[XSDK_FORMAT_DXT3] = { 19, 2, 4, ENDIAN_8IN16, SWIZZLE(X, Y, Z, W) },
	[XSDK_FORMAT_DXT5] = { 20, 2, 4, ENDIAN_8IN16, SWIZZLE(X, Y, Z, W) },
	[XSDK_FORMAT_R8G8B8A8] = { 6, 0, 2, ENDIAN_NONE, SWIZZLE(X, Y, Z, W) },
	[XSDK_FORMAT_B8G8R8A8] = { 6, 0, 2, ENDIAN_NONE, SWIZZLE(Z, Y, X, W) },
	// k_32_FLOAT (xenia:src/xenia/gpu/xenos.h:512).
	[XSDK_FORMAT_R32F] = { 36, 0, 2, ENDIAN_8IN32, SWIZZLE(X, ZERO, ZERO, ONE) },
};

static NTSTATUS reject(const char *why)
{
	xsdk_log_write("texture: %s", why);
	return STATUS_INVALID_PARAMETER;
}

static uint32_t round_up(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
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

static TextureLayout layout_of(const XsdkTexture *texture)
{
	const FormatInfo *const info = &formats[texture->format];
	return (TextureLayout){ .width = texture->width,
		.height = texture->height,
		.levels = texture->levels,
		.block_log2 = info->block_log2,
		.block_bytes_log2 = info->block_bytes_log2,
		.base_pitch_blocks = texture->pitch >> info->block_log2,
		.mip_offset_bytes = texture->mip_offset_bytes };
}

// Whole tiles of blocks; linear rows also start every 256 bytes.
static uint32_t base_pitch_blocks(const FormatInfo *info, uint32_t width, bool tiled)
{
	const uint32_t blocks = round_up(xsdk_texture_blocks(width, info->block_log2), TILE_BLOCKS);
	if (tiled)
		return blocks;
	return round_up(blocks << info->block_bytes_log2, LINEAR_ROW_ALIGNMENT_BYTES) >> info->block_bytes_log2;
}

// Xe_CreateTexture and TEXTURE_FETCH for the format's levels, the base then the mips, a page apart.
NTSTATUS xsdk_texture_init(
	XsdkTexture *texture, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, bool tiled)
{
	if (format >= XSDK_FORMATS)
		return reject("format not one of XSDK_FORMAT_*");
	if (!width || !height || width > XSDK_TEXTURE_MAX_SIZE || height > XSDK_TEXTURE_MAX_SIZE)
		return reject("texture size not from 1 to 8192");
	if (!levels || levels > level_count(width, height))
		return reject("levels not from 1 to a 1x1 level");
	if (!tiled && levels > 1) {
		xsdk_log_write("texture: linear textures have one level");
		return STATUS_NOT_IMPLEMENTED;
	}
	const FormatInfo *const info = &formats[format];
	const uint32_t pitch_blocks = base_pitch_blocks(info, width, tiled);
	const uint32_t rows = round_up(xsdk_texture_blocks(height, info->block_log2), TILE_BLOCKS);
	const uint32_t mip_offset_bytes = round_up(pitch_blocks * rows << info->block_bytes_log2, PAGE_BYTES);
	const TextureLayout layout = { .width = width,
		.height = height,
		.levels = levels,
		.block_log2 = info->block_log2,
		.block_bytes_log2 = info->block_bytes_log2,
		.base_pitch_blocks = pitch_blocks,
		.mip_offset_bytes = mip_offset_bytes };
	const uint32_t size_bytes = mip_offset_bytes + xsdk_texture_mip_bytes(&layout);
	const uint32_t pitch = pitch_blocks << info->block_log2;
	*texture = (XsdkTexture){
		.size_bytes = size_bytes,
		.format = format,
		.width = width,
		.height = height,
		.levels = levels,
		.pitch = pitch,
		.tiled = tiled,
		.mip_offset_bytes = mip_offset_bytes,
		.fetch = {
			FETCH_TYPE_TEXTURE | (pitch >> PITCH_UNIT_LOG2) << PITCH_SHIFT | (uint32_t)tiled << TILED_SHIFT,
			info->endian << ENDIAN_SHIFT | info->format,
			(height - 1) << HEIGHT_SHIFT | (width - 1),
			info->swizzle << SWIZZLE_SHIFT,
			(levels - 1) << MIP_MAX_LEVEL_SHIFT,
			PACKED_MIPS | DIMENSION_2D,
		},
	};
	return STATUS_SUCCESS;
}

void xsdk_texture_attach(XsdkTexture *texture, volatile uint32_t *texels)
{
	const uint32_t address = xsdk_gpu_address(texels);
	assert(address % FETCH_ADDRESS_ALIGNMENT == 0);
	// Mips in the base's tail point back at the base, so Xenia reads them where the console does.
	const bool mips_apart = texture->size_bytes > texture->mip_offset_bytes;
	const uint32_t mip_address = texture->levels == 1 ? 0 : mips_apart ? address + texture->mip_offset_bytes : address;
	texture->texels = texels;
	texture->address = address;
	texture->fetch[1] |= address;
	texture->fetch[5] |= mip_address;
}

NTSTATUS xsdk_texture_create(
	XsdkTexture *texture, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, bool tiled)
{
	const NTSTATUS status = xsdk_texture_init(texture, format, width, height, levels, tiled);
	if (!NT_SUCCESS(status))
		return status;
	volatile uint32_t *const texels = xsdk_gpu_alloc(texture->size_bytes);
	if (!texels)
		return STATUS_NO_MEMORY;
	xsdk_texture_attach(texture, texels);
	return STATUS_SUCCESS;
}

// A block's byte offset in its stored image.
static uint32_t block_offset(const XsdkTexture *texture, const TextureLevelPlace *place, uint32_t x, uint32_t y)
{
	const uint32_t bytes_log2 = formats[texture->format].block_bytes_log2;
	if (texture->tiled)
		return xsdk_tiled_2d(x, y, place->pitch_blocks, bytes_log2);
	return (y * place->pitch_blocks + x) << bytes_log2;
}

// Bytes within one 32-bit word, merged with what the word holds unless they fill it: reading the word back from
// write-combined memory goes all the way to memory.
static void put_bytes(volatile uint32_t *image, uint32_t offset, const uint8_t *bytes, uint32_t count)
{
	volatile uint32_t *const word = image + offset / WORD_BYTES;
	uint32_t value = count == WORD_BYTES ? 0 : *word;
	memcpy((uint8_t *)&value + offset % WORD_BYTES, bytes, count);
	*word = value;
}

// Whole words from word-aligned bytes to a word-aligned offset, each one store.
static void put_words(volatile uint32_t *image, uint32_t offset, const uint8_t *bytes, uint32_t count)
{
	volatile uint32_t *word = image + offset / WORD_BYTES;
	for (uint32_t i = 0; i < count; i += WORD_BYTES) {
		uint32_t value;
		memcpy(&value, bytes + i, WORD_BYTES);
		*word++ = value;
	}
}

// One block, a word or part of one at a time: blocks of 4 bytes or more fill whole words, smaller ones share a word
// with their neighbours.
static void put_block(volatile uint32_t *image, uint32_t offset, const uint8_t *bytes, uint32_t block_bytes)
{
	for (uint32_t byte = 0; byte < block_bytes; byte += WORD_BYTES)
		put_bytes(image, offset + byte, bytes + byte, block_bytes < WORD_BYTES ? block_bytes : WORD_BYTES);
}

// How many blocks in a row go as one run from a block whose x is a multiple of it: in a linear row, the whole row cut
// to whole words.
static uint32_t run_blocks(const XsdkTexture *texture, const FormatInfo *info, uint32_t columns)
{
	if (!texture->tiled) {
		const uint32_t per_word = info->block_bytes_log2 >= 2 ? 1 : WORD_BYTES >> info->block_bytes_log2;
		const uint32_t whole = columns / per_word * per_word;
		return whole ? whole : 1;
	}
	const uint32_t blocks = TILED_RUN_BYTES >> info->block_bytes_log2;
	return blocks == 0 ? 1 : blocks > TILED_RUN_BLOCKS_MAX ? TILED_RUN_BLOCKS_MAX : blocks;
}

void xsdk_texture_upload(XsdkTexture *texture, uint32_t level, const void *data, uint32_t row_bytes)
{
	assert(level < texture->levels);
	const FormatInfo *const info = &formats[texture->format];
	const TextureLayout layout = layout_of(texture);
	TextureLevelPlace place;
	xsdk_texture_place(&layout, level, &place);
	volatile uint32_t *const image = texture->texels + place.offset_bytes / WORD_BYTES;
	const uint32_t block_bytes = 1u << info->block_bytes_log2;
	const uint32_t columns = xsdk_texture_blocks(level_size(texture->width, level), info->block_log2);
	const uint32_t rows = xsdk_texture_blocks(level_size(texture->height, level), info->block_log2);
	// A run goes as whole words, read from the rows as words, so they must be word-aligned; unaligned rows go a block
	// at a time.
	const uint32_t run = ((uintptr_t)data | row_bytes) % WORD_BYTES == 0 ? run_blocks(texture, info, columns) : 1;
	const uint32_t run_bytes = run * block_bytes;
	for (uint32_t y = 0; y < rows; y++) {
		const uint8_t *const row = (const uint8_t *)data + y * row_bytes;
		uint32_t x = 0;
		while (x < columns) {
			const uint32_t offset = block_offset(texture, &place, place.x + x, place.y + y);
			// A whole run, starting where the layout's runs start, of whole words: at a packed level's edges and
			// across a linear row's last bytes, blocks go one at a time.
			if ((place.x + x) % run == 0 && x + run <= columns && run_bytes % WORD_BYTES == 0 &&
				offset % WORD_BYTES == 0) {
				put_words(image, offset, row + x * block_bytes, run_bytes);
				x += run;
				continue;
			}
			put_block(image, offset, row + x * block_bytes, block_bytes);
			x++;
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

// Where the GPU takes an XSDK_CHANNEL_* from, among the swizzle sources, given the format's own swizzle.
static uint32_t channel_source(uint32_t own, uint32_t channel)
{
	if (channel == XSDK_CHANNEL_ZERO)
		return ZERO;
	if (channel == XSDK_CHANNEL_ONE)
		return ONE;
	return own >> channel * SWIZZLE_BITS & 7;
}

void xsdk_texture_swizzle(XsdkTexture *texture, const uint32_t channels[4])
{
	const uint32_t own = formats[texture->format].swizzle;
	uint32_t swizzle = 0;
	for (uint32_t i = 0; i < 4; i++) {
		assert(channels[i] <= XSDK_CHANNEL_ONE);
		swizzle |= channel_source(own, channels[i]) << i * SWIZZLE_BITS;
	}
	texture->fetch[3] = (texture->fetch[3] & ~((uint32_t)SWIZZLE_MASK << SWIZZLE_SHIFT)) | swizzle << SWIZZLE_SHIFT;
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
