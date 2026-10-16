// xsdk_tiled_2d and xsdk_tiled_3d come from xenia:src/xenia/gpu/texture_address.h:174-238, rewritten in C for positive
// coordinates.
// Xenia carries this notice:
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

#include <assert.h>
#include <stdint.h>

#include "gpu_private.h"

enum {
	MACRO_TILE_LOG2 = 5,
	MACRO_TILE_TEXELS = 1 << MACRO_TILE_LOG2,
	MACRO_TILE_BLOCKS_LOG2 = 6,
	// A 3D macro tile is 32 by 16 by 4 blocks.
	MACRO_TILE_HEIGHT_3D_LOG2 = 4,
	MACRO_TILE_DEPTH_3D_LOG2 = 2,
	MACRO_TILE_BLOCKS_3D_LOG2 = 7,
};

// TiledCombine: spreads the offset's bits around the bank, pipe and row bits.
static uint32_t combine(uint32_t outer_inner_bytes, uint32_t bank, uint32_t pipe, uint32_t y_lsb)
{
	return y_lsb << 4 | pipe << 6 | bank << 11 | (outer_inner_bytes & 0xF) | (outer_inner_bytes >> 4 & 1) << 5 |
		(outer_inner_bytes >> 5 & 7) << 8 | (outer_inner_bytes >> 8) << 12;
}

uint32_t xsdk_tiled_2d(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_texel_log2)
{
	assert(pitch_texels % MACRO_TILE_TEXELS == 0);
	const uint32_t outer_blocks = ((y >> MACRO_TILE_LOG2) * (pitch_texels >> MACRO_TILE_LOG2) + (x >> MACRO_TILE_LOG2))
		<< MACRO_TILE_BLOCKS_LOG2;
	const uint32_t inner_blocks = (y >> 1 & 7) << 3 | (x & 7);
	const uint32_t bank = y >> 4 & 1;
	const uint32_t pipe = (x >> 3 & 3) ^ (y >> 3 & 1) << 1;
	return combine((outer_blocks | inner_blocks) << bytes_per_texel_log2, bank, pipe, y & 1);
}

uint32_t xsdk_tiled_3d(
	uint32_t x, uint32_t y, uint32_t z, uint32_t pitch_texels, uint32_t height_texels, uint32_t bytes_per_texel_log2)
{
	assert(pitch_texels % MACRO_TILE_TEXELS == 0 && height_texels % MACRO_TILE_TEXELS == 0);
	const uint32_t tile_rows = (z >> MACRO_TILE_DEPTH_3D_LOG2) * (height_texels >> MACRO_TILE_HEIGHT_3D_LOG2) +
		(y >> MACRO_TILE_HEIGHT_3D_LOG2);
	const uint32_t outer_blocks = (tile_rows * (pitch_texels >> MACRO_TILE_LOG2) + (x >> MACRO_TILE_LOG2))
		<< MACRO_TILE_BLOCKS_3D_LOG2;
	const uint32_t inner_blocks = (z & 3) << 5 | (y >> 1 & 3) << 3 | (x & 7);
	// Every other four slices flip the bank, and the pipe's high bit with it.
	const uint32_t bank = (y >> 3 ^ z >> 2) & 1;
	const uint32_t pipe = (x >> 3 & 3) ^ bank << 1;
	return combine((outer_blocks | inner_blocks) << bytes_per_texel_log2, bank, pipe, y & 1);
}
