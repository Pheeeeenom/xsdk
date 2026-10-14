// The mip layout below comes from xenia:src/xenia/gpu/texture_util.cc:116-207, 209-449 and texture_util.h:41-44,
// rewritten in C for 2D textures. Xenia carries this notice:
//
// Copyright (c) 2015, Ben Vanik.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the project nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdbool.h>
#include <stdint.h>

#include "gpu_private.h"

enum {
	// Tiles and stored rows go in 32 by 32 blocks (xenia:src/xenia/gpu/texture_util.cc:304-345).
	TILE_BLOCKS = 32,
	// Each stored level starts on a page (xenia:src/xenia/gpu/xenos.h:1132-1134).
	PAGE_BYTES = 0x1000,
	// Levels 16 texels or smaller across their short side share one tail.
	PACKED_SIZE_LOG2 = 4,
	// The tail's first three levels go side by side; later ones step down the other way.
	PACKED_SIDE_BY_SIDE = 3,
};

static uint32_t log2_ceil(uint32_t value)
{
	return value <= 1 ? 0 : 32 - (uint32_t)__builtin_clz(value - 1);
}

static uint32_t round_up(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

uint32_t xsdk_texture_packed_level(uint32_t width, uint32_t height)
{
	const uint32_t size_log2 = log2_ceil(width < height ? width : height);
	return size_log2 > PACKED_SIZE_LOG2 ? size_log2 - PACKED_SIZE_LOG2 : 0;
}

uint32_t xsdk_texture_blocks(uint32_t texels, uint32_t block_log2)
{
	return (texels + (1u << block_log2) - 1) >> block_log2;
}

// Mips are stored as if the base were a power of two, in whole tiles of blocks.
static uint32_t stored_blocks(uint32_t texels, uint32_t stored_level, uint32_t block_log2)
{
	const uint32_t level_texels = (1u << log2_ceil(texels)) >> stored_level;
	return round_up(xsdk_texture_blocks(level_texels ? level_texels : 1, block_log2), TILE_BLOCKS);
}

static uint32_t stored_bytes(const TextureLayout *layout, uint32_t stored_level)
{
	const uint32_t blocks = stored_blocks(layout->width, stored_level, layout->block_log2) *
		stored_blocks(layout->height, stored_level, layout->block_log2);
	return round_up(blocks << layout->block_bytes_log2, PAGE_BYTES);
}

// A level's texels from the tail's corner, laid out by whether the texture is wider than tall. One texel high counts
// as two there: the console read a 2x1 texture's base at (16, 0), as a square's, and a 4x1's at (0, 16), as a wide
// one's, where Xenia lays out 2x1 as wide too (docs/HARDWARE.md, "A 2x1 texture's tail").
static void packed_offset(uint32_t width, uint32_t height, uint32_t level, uint32_t *x, uint32_t *y)
{
	const uint32_t width_log2 = log2_ceil(width);
	const uint32_t height_log2 = log2_ceil(height);
	const uint32_t packed_level = xsdk_texture_packed_level(width, height);
	const uint32_t step = level - packed_level;
	const bool wide = width_log2 > (height_log2 > 1 ? height_log2 : 1);
	if (step < PACKED_SIDE_BY_SIDE) {
		*x = wide ? 0 : (1u << PACKED_SIZE_LOG2) >> step;
		*y = wide ? (1u << PACKED_SIZE_LOG2) >> step : 0;
		return;
	}
	const uint32_t offset = (1u << ((wide ? width_log2 : height_log2) - packed_level)) >> (step - 2);
	*x = wide ? offset : 0;
	*y = wide ? 0 : offset;
}

uint32_t xsdk_texture_mip_bytes(const TextureLayout *layout)
{
	const uint32_t packed_level = xsdk_texture_packed_level(layout->width, layout->height);
	// With the base in the tail, the console reads every level from it, whatever the mip address (docs/HARDWARE.md).
	if (layout->levels <= 1 || packed_level == 0)
		return 0;
	const uint32_t last = layout->levels - 1 < packed_level ? layout->levels - 1 : packed_level;
	uint32_t bytes = 0;
	for (uint32_t stored = 1; stored <= last; stored++)
		bytes += stored_bytes(layout, stored);
	return bytes;
}

void xsdk_texture_place(const TextureLayout *layout, uint32_t level, TextureLevelPlace *place)
{
	const uint32_t packed_level = xsdk_texture_packed_level(layout->width, layout->height);
	*place = (TextureLevelPlace){ .offset_bytes = 0, .pitch_blocks = layout->base_pitch_blocks, .x = 0, .y = 0 };
	if (level >= packed_level) {
		uint32_t x;
		uint32_t y;
		packed_offset(layout->width, layout->height, level, &x, &y);
		place->x = x >> layout->block_log2;
		place->y = y >> layout->block_log2;
	}
	if (level == 0 || packed_level == 0)
		return;
	const uint32_t stored = level < packed_level ? level : packed_level;
	place->offset_bytes = layout->mip_offset_bytes;
	for (uint32_t before = 1; before < stored; before++)
		place->offset_bytes += stored_bytes(layout, before);
	place->pitch_blocks = stored_blocks(layout->width, stored, layout->block_log2);
}
