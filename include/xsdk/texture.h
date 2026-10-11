#ifndef XSDK_TEXTURE_H
#define XSDK_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// 32-bit ARGB textures in GPU memory, with mip levels when tiled, sampled as a sampler state says.

enum {
	XSDK_TEXTURE_MAX_SIZE = 8192,
	XSDK_TEXTURE_FETCH_DWORDS = 6,
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
	uint32_t width;
	uint32_t height;
	uint32_t levels;
	// Texels from one row to the next of the base level; tiled textures go in 32 by 32 tiles.
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

// Allocates the texture, linear or in Xenos's tiled layout, with levels from 1 to a 1x1 level; linear has 1.
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t width, uint32_t height, uint32_t levels, bool tiled);
// Copies in a level's ARGB pixels, row_pixels apart, halving each size per level; not while the GPU may read it.
void xsdk_texture_upload(XsdkTexture *texture, uint32_t level, const uint32_t *argb, uint32_t row_pixels);
void xsdk_texture_free(XsdkTexture *texture);
// Works out the sampler's fetch constant bits once, for xsdk_gpu_set_texture.
void xsdk_texture_sampler_state(XsdkSamplerState *state, const XsdkSampler *sampler);

#ifdef __cplusplus
}
#endif

#endif
