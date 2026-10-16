#ifndef XSDK_TEXTURE_H
#define XSDK_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Textures in GPU memory, with mip levels when tiled, sampled as a sampler state says.

enum {
	XSDK_TEXTURE_MAX_SIZE = 8192,
	// A 3D texture's sides and slices (xenia:src/xenia/gpu/xenos.h:1144-1147).
	XSDK_TEXTURE_3D_MAX_SIZE = 2048,
	XSDK_TEXTURE_3D_MAX_DEPTH = 1024,
	// A cube's faces, +x, -x, +y, -y, +z then -z, as the cube instruction numbers them
	// (xenia:src/xenia/gpu/spirv_shader_translator_alu.cc:583-640).
	XSDK_CUBE_FACES = 6,
	XSDK_TEXTURE_FETCH_DWORDS = 6,
	XSDK_MAX_ANISOTROPY = 16,
};

// The fetch constant's DataDimension values (xenia:src/xenia/gpu/xenos.h:97-102).
enum {
	XSDK_TEXTURE_2D = 1,
	XSDK_TEXTURE_3D = 2,
	XSDK_TEXTURE_CUBE = 3,
};

// Formats, read as Vulkan's red, green, blue and alpha; uploads take each format's units in the CPU's order.
enum {
	// 32-bit units, 0xAARRGGBB.
	XSDK_FORMAT_A8R8G8B8,
	// 16-bit units, red in the top bits.
	XSDK_FORMAT_R5G6B5,
	XSDK_FORMAT_A1R5G5B5,
	XSDK_FORMAT_A4R4G4B4,
	// 32-bit units, red in the low bits, alpha in the top two.
	XSDK_FORMAT_A2B10G10R10,
	// Bytes: red, or red then green.
	XSDK_FORMAT_R8,
	XSDK_FORMAT_R8G8,
	// 16-bit half floats: red; red then green; or red, green, blue then alpha.
	XSDK_FORMAT_R16F,
	XSDK_FORMAT_R16G16F,
	XSDK_FORMAT_R16G16B16A16F,
	// 4 by 4 blocks as 16-bit words, in the order of a .dds file's little-endian words (Vulkan's BC1 to BC3).
	XSDK_FORMAT_DXT1,
	XSDK_FORMAT_DXT3,
	XSDK_FORMAT_DXT5,
	// Bytes: red, green, blue then alpha; or blue, green, red then alpha (Vulkan's R8G8B8A8 and B8G8R8A8).
	XSDK_FORMAT_R8G8B8A8,
	XSDK_FORMAT_B8G8R8A8,
	// 32-bit floats: red (Vulkan's R32_SFLOAT).
	XSDK_FORMAT_R32F,
	// 32-bit units, depth's 24 bits above stencil's 8, as xsdk_gpu_resolve_depth_texture writes them; sampled, the
	// depth from 0 to 1 in red (Vulkan's D24_UNORM_S8_UINT, its depth).
	XSDK_FORMAT_D24S8,
	// 4 by 4 blocks as 16-bit words, as the DXT formats: red and green, each a DXT5 alpha block (Vulkan's BC5), or red
	// alone (Vulkan's BC4).
	XSDK_FORMAT_DXN,
	XSDK_FORMAT_DXT5A,
	// 16-bit units, 0 to 1: red; red then green; or red, green, blue then alpha.
	XSDK_FORMAT_R16,
	XSDK_FORMAT_R16G16,
	XSDK_FORMAT_R16G16B16A16,
	// 32-bit units, 0 to 1, red in the low bits: 11, 11 and 10 bits of red, green and blue; or 10, 11 and 11.
	XSDK_FORMAT_B10G11R11,
	XSDK_FORMAT_B11G11R10,
	XSDK_FORMATS,
};

// What xsdk_texture_swizzle has the GPU return: one of the format's channels, as the list above reads them, or 0 or 1.
enum {
	XSDK_CHANNEL_RED,
	XSDK_CHANNEL_GREEN,
	XSDK_CHANNEL_BLUE,
	XSDK_CHANNEL_ALPHA,
	XSDK_CHANNEL_ZERO,
	XSDK_CHANNEL_ONE,
};

// How xsdk_texture_signs has the GPU read a channel: from 0 to 1; signed, two's complement from -1 to 1, the most
// negative and the next both -1; or gamma, from 0 to 1 through Xenos's piecewise-linear curve, near sRGB's
// (xenia:src/xenia/gpu/xenos.h:121-129, xenos.cc:27).
enum {
	XSDK_SIGN_UNSIGNED,
	XSDK_SIGN_SIGNED,
	XSDK_SIGN_GAMMA,
};

// The fetch constant's TextureFilter values (xenia:src/xenia/gpu/xenos.h:131-138).
enum {
	XSDK_FILTER_POINT = 0,
	XSDK_FILTER_LINEAR = 1,
};

// The fetch constant's ClampMode values (xenia:src/xenia/gpu/xenos.h:104-113).
enum {
	XSDK_ADDRESS_WRAP = 0,
	XSDK_ADDRESS_MIRROR = 1,
	XSDK_ADDRESS_CLAMP = 2,
};

typedef struct {
	volatile uint32_t *texels;
	uint32_t address;
	uint32_t size_bytes;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t levels;
	// XSDK_TEXTURE_2D, _CUBE or _3D, and its faces or slices: 1 for 2D.
	uint32_t dimension;
	uint32_t depth;
	// Texels from one row to the next of the base level; tiled textures go in tiles of 32 by 32 texels or blocks.
	uint32_t pitch;
	bool tiled;
	// Levels 1 on, from this many bytes in.
	uint32_t mip_offset_bytes;
	// The texture's half of the fetch constant xsdk_gpu_set_texture writes; the sampler state fills the rest.
	uint32_t fetch[XSDK_TEXTURE_FETCH_DWORDS];
} XsdkTexture;

typedef struct {
	uint32_t mag_filter;
	uint32_t min_filter;
	uint32_t mip_filter;
	uint32_t address_u;
	uint32_t address_v;
	// Added to the level the GPU works out, from -16 to under 16, in steps of 1/32.
	float lod_bias;
	// The most anisotropy, 1 to 16, rounded down to a power of two; 0 is none.
	uint32_t max_anisotropy;
	// The finest level sampled, from 0 to 15, and how many levels from the base it may sample, from 1 to 16, or 0 for
	// all the texture's: Xenos clamps to whole levels (xenia:src/xenia/gpu/xenos.h:1265-1266, mip_min_level and
	// mip_max_level). The sampled levels end at the texture's last whichever is fewer.
	uint32_t min_level;
	uint32_t level_count;
} XsdkSampler;

typedef struct {
	// The sampler's half of the fetch constant.
	uint32_t fetch[XSDK_TEXTURE_FETCH_DWORDS];
} XsdkSamplerState;

// Works out the texture's layout and size_bytes, without memory: linear or in Xenos's tiled layout, with levels from 1
// to a 1x1 level; linear has 1.
NTSTATUS xsdk_texture_init(
	XsdkTexture *texture, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, bool tiled);
// Puts an initialised texture's texels at memory from xsdk_gpu_alloc, 4 KB aligned and size_bytes long.
void xsdk_texture_attach(XsdkTexture *texture, volatile uint32_t *texels);
// xsdk_texture_init, then memory of its own for xsdk_texture_attach.
NTSTATUS xsdk_texture_create(
	XsdkTexture *texture, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, bool tiled);
// Copies in a level's rows of texels or blocks, row_bytes apart, sizes halving per level; not while the GPU reads it.
// Word-aligned rows go in runs of whole words, far faster than unaligned ones.
void xsdk_texture_upload(XsdkTexture *texture, uint32_t level, const void *data, uint32_t row_bytes);
// The same for row_count rows of blocks from first_row, data holding the first of them.
void xsdk_texture_upload_rows(
	XsdkTexture *texture, uint32_t level, uint32_t first_row, uint32_t row_count, const void *data, uint32_t row_bytes);
// Copies rows out to data as the uploads take them, a store at a time, so data may be write-combined memory; not while
// the GPU writes the texture. It reads write-combined memory, so it's slow.
void xsdk_texture_download_rows(
	const XsdkTexture *texture, uint32_t level, uint32_t first_row, uint32_t row_count, void *data, uint32_t row_bytes);
// xsdk_texture_init for a cube of six square faces size texels on a side, tiled, with levels from 1 to a 1x1 level,
// each face's laid out as a 2D texture's (xenia:src/xenia/gpu/texture_util.cc:209-449).
NTSTATUS xsdk_texture_init_cube(XsdkTexture *texture, uint32_t format, uint32_t size, uint32_t levels);
// xsdk_texture_init for a 3D texture depth slices deep, tiled, of one level.
NTSTATUS xsdk_texture_init_3d(XsdkTexture *texture, uint32_t format, uint32_t width, uint32_t height, uint32_t depth);
// xsdk_texture_upload_rows and xsdk_texture_download_rows for one face of a cube, from 0 to 5, or one slice of a 3D
// texture; slice 0 of a 2D texture is the texture.
void xsdk_texture_upload_slice_rows(XsdkTexture *texture, uint32_t level, uint32_t slice, uint32_t first_row,
	uint32_t row_count, const void *data, uint32_t row_bytes);
void xsdk_texture_download_slice_rows(const XsdkTexture *texture, uint32_t level, uint32_t slice, uint32_t first_row,
	uint32_t row_count, void *data, uint32_t row_bytes);
// Has the texture return, as red, green, blue and alpha, the XSDK_CHANNEL_* each names; xsdk_texture_init returns
// the format's own channels.
void xsdk_texture_swizzle(XsdkTexture *texture, const uint32_t channels[4]);
// Has the GPU read the format's red, green, blue and alpha, as the format list reads them, as the XSDK_SIGN_* each
// names; xsdk_texture_init reads them unsigned. Swizzles pick among the channels as read.
void xsdk_texture_signs(XsdkTexture *texture, const uint32_t signs[4]);
// Frees what xsdk_texture_create allocated.
void xsdk_texture_free(XsdkTexture *texture);
// Works out the sampler's fetch constant bits once, for xsdk_gpu_set_texture.
void xsdk_texture_sampler_state(XsdkSamplerState *state, const XsdkSampler *sampler);

#ifdef __cplusplus
}
#endif

#endif
