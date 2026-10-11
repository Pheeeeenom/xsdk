#ifndef XSDK_PRESENT_H
#define XSDK_PRESENT_H

#include <stdint.h>

#include "xsdk/display.h"
#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Frames shown at vblank through the graphics interrupt callback, in order, from two or three front buffers.

enum {
	XSDK_PRESENT_MAX_BUFFERS = 3,
};

typedef struct {
	uint32_t vblanks;
	uint32_t flips;
	// Vblanks where a frame was due and none had finished, so the last one showed again.
	uint32_t missed;
	// Flips whose writes ran past the end of vblank, where they could tear.
	uint32_t late;
	uint32_t first_flip_vblank;
	uint32_t last_flip_vblank;
	// The longest xsdk_present_begin waited for a front buffer to come off screen.
	uint32_t longest_wait_us;
} XsdkPresentStats;

// Opens the display in this mode with buffer_count front buffers, 2 or 3; each frame shows for swap_interval vblanks.
NTSTATUS xsdk_present_open(
	XsdkDisplayMode mode, uint32_t buffer_count, uint32_t swap_interval, uint32_t *width, uint32_t *height);
// Waits up to timeout_ms for the next front buffer to come off screen, then gives its address to resolve into.
NTSTATUS xsdk_present_begin(uint32_t *address, uint32_t timeout_ms);
// The CPU's view of the front buffer at an address xsdk_present_begin gave.
volatile uint32_t *xsdk_present_pixels(uint32_t address);
// Queues the frame resolved into that buffer; the vblank callback shows it once the GPU has finished it.
void xsdk_present_end(void);
void xsdk_present_stats(XsdkPresentStats *stats);
// Waits for the queued frames to show, stops flipping, closes the display and frees the buffers.
void xsdk_present_close(void);

#ifdef __cplusplus
}
#endif

#endif
