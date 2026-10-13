#ifndef XSDK_MICROCODE_PRIVATE_H
#define XSDK_MICROCODE_PRIVATE_H

#include <stdint.h>

// Shaders' microcode, in 256-byte blocks of shared 64 KiB pages; xsdk_gpu_alloc would give each shader a page.
void xsdk_microcode_init(void);
volatile uint32_t *xsdk_microcode_alloc(uint32_t size_bytes);
// size_bytes is what xsdk_microcode_alloc was asked for; memory may be NULL.
void xsdk_microcode_free(volatile uint32_t *memory, uint32_t size_bytes);

#endif
