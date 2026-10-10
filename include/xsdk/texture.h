#ifndef XSDK_TEXTURE_H
#define XSDK_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// 32-bit ARGB textures in GPU memory, sampled at the nearest texel with their edges clamped.

enum {
	XSDK_TEXTURE_MAX_SIZE = 8192,
	XSDK_TEXTURE_FETCH_DWORDS = 6,
};

typedef struct {
	volatile uint32_t *texels;
	uint32_t address;
	uint32_t size_bytes;
	uint32_t width;
	uint32_t height;
	// Texels from one row to the next; tiled textures go in 32 by 32 tiles.
	uint32_t pitch;
	bool tiled;
	// The texture fetch constant xsdk_gpu_set_texture writes.
	uint32_t fetch[XSDK_TEXTURE_FETCH_DWORDS];
} XsdkTexture;

// Allocates the texture, linear or in Xenos's tiled layout.
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t width, uint32_t height, bool tiled);
// Copies in the texture's width by height ARGB pixels, row_pixels apart; not while the GPU may be reading it.
void xsdk_texture_upload(XsdkTexture *texture, const uint32_t *argb, uint32_t row_pixels);
void xsdk_texture_free(XsdkTexture *texture);

#ifdef __cplusplus
}
#endif

#endif
