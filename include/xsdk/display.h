#ifndef XSDK_DISPLAY_H
#define XSDK_DISPLAY_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// The display controller, which scans a front buffer out to the screen. Take the GPU first (docs/HARDWARE.md).

// Takes the display and returns its mode's size; STATUS_NOT_SUPPORTED where there is none, as in Xenia.
NTSTATUS xsdk_display_open(uint32_t *width, uint32_t *height);
// Shows the front buffer at this GPU address, in the tiled k_8_8_8_8 layout xsdk_gpu_resolve writes.
NTSTATUS xsdk_display_show(uint32_t address, uint32_t pitch_pixels);
// Shows XAM's surface again. The title's exit calls it too.
void xsdk_display_close(void);

#ifdef __cplusplus
}
#endif

#endif
