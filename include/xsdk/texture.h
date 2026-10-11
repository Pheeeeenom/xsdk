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
	XSDK_TEXTURE_FETCH_DWORDS = 6,
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
	XSDK_FORMATS,
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
void xsdk_texture_upload(XsdkTexture *texture, uint32_t level, const void *data, uint32_t row_bytes);
// Frees what xsdk_texture_create allocated.
void xsdk_texture_free(XsdkTexture *texture);
// Works out the sampler's fetch constant bits once, for xsdk_gpu_set_texture.
void xsdk_texture_sampler_state(XsdkSamplerState *state, const XsdkSampler *sampler);

#ifdef __cplusplus
}
#endif

#endif
