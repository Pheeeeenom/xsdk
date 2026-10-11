#ifndef XSDK_DISPLAY_H
#define XSDK_DISPLAY_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// The display controller, which scans a front buffer out to the screen. Take the GPU first (docs/HARDWARE.md).

typedef enum {
	// 1280x720, which the display scales to the console's output; on an interlaced output, the kernel's mode instead.
	XSDK_DISPLAY_720P,
	// The kernel's own front buffer and scaling, 640x480 on the console tested.
	XSDK_DISPLAY_KERNEL,
} XsdkDisplayMode;

// Takes the display and returns the front buffer's size; STATUS_NOT_SUPPORTED where there is none, as in Xenia.
NTSTATUS xsdk_display_open(XsdkDisplayMode mode, uint32_t *width, uint32_t *height);
// Shows the front buffer at this GPU address, in the tiled k_8_8_8_8 layout xsdk_gpu_resolve writes.
NTSTATUS xsdk_display_show(uint32_t address, uint32_t pitch_pixels);
// Shows XAM's surface again at the kernel's scaling. The title's exit calls it too.
void xsdk_display_close(void);

#ifdef __cplusplus
}
#endif

#endif
