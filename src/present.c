#include "xsdk/present.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "gpu_private.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/thread.h"

enum {
	// The line being scanned, and the interrupt status, bit 0 for vblank
	// (xenia:src/xenia/gpu/graphics_system.cc:189-192).
	R500_D1MODE_V_COUNTER = 0x194C,
	UNKNOWN_1951 = 0x1951,
	// Of the callback's two calls a frame, only the vblank one has it (docs/HARDWARE.md).
	VBLANK_INTERRUPT = 1,
	// Frame k's fence goes in slot k % QUEUE_SLOTS; no more than buffer_count - 1 frames wait to show.
	QUEUE_SLOTS = 4,
	CLOSE_TIMEOUT_MS = 1000,
	// Opaque black, as the display reads a word (docs/HARDWARE.md, "A title's buffer on screen").
	BLACK = 0x000000FF,
	MICROSECONDS_PER_SECOND = 1000000,
};

typedef struct {
	volatile uint32_t *buffers[XSDK_PRESENT_MAX_BUFFERS];
	uint32_t addresses[XSDK_PRESENT_MAX_BUFFERS];
	uint32_t buffer_count;
	uint32_t swap_interval;
	uint32_t height;
	uint32_t longest_wait_us;
	bool open;
	// Written by the title.
	volatile uint32_t queued;
	volatile uint32_t fences[QUEUE_SLOTS];
	volatile bool stopping;
	// Written by the callback.
	volatile uint32_t flipped;
	volatile uint32_t vblanks;
	volatile uint32_t missed;
	volatile uint32_t late;
	volatile uint32_t first_flip_vblank;
	volatile uint32_t last_flip_vblank;
} Present;

static Present present;

// From the mode's last line on, the display is in vblank.
static bool in_vblank(void)
{
	return gpu_registers[R500_D1MODE_V_COUNTER] >= present.height;
}

// Runs in the kernel's interrupt handling, twice a frame; it only reads registers, flips and counts.
static void on_interrupt(uint32_t source, void *context)
{
	(void)source;
	(void)context;
	if (!(gpu_registers[UNKNOWN_1951] & VBLANK_INTERRUPT))
		return;
	const uint32_t vblank = present.vblanks + 1;
	present.vblanks = vblank;
	if (present.flipped && vblank - present.last_flip_vblank < present.swap_interval)
		return;
	const uint32_t next = present.flipped;
	// Order the fence's read after the count that published it.
	__asm__ volatile("lwsync" ::: "memory");
	if (next == present.queued || !xsdk_gpu_fence_done(present.fences[next % QUEUE_SLOTS])) {
		if (next && !present.stopping)
			present.missed = present.missed + 1;
		return;
	}
	xsdk_display_flip(present.addresses[next % present.buffer_count]);
	if (!in_vblank())
		present.late = present.late + 1;
	if (!next)
		present.first_flip_vblank = vblank;
	present.last_flip_vblank = vblank;
	__asm__ volatile("lwsync" ::: "memory");
	present.flipped = next + 1;
}

static uint32_t elapsed_us(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * MICROSECONDS_PER_SECOND + (now.tv_nsec - start->tv_nsec) / 1000);
}

NTSTATUS xsdk_present_open(uint32_t buffer_count, uint32_t swap_interval, uint32_t *width, uint32_t *height)
{
	assert(!present.open && buffer_count >= 2 && buffer_count <= XSDK_PRESENT_MAX_BUFFERS);
	assert(swap_interval >= 1 && swap_interval <= 2);
	const NTSTATUS status = xsdk_display_open(width, height);
	if (!NT_SUCCESS(status))
		return status;
	present = (Present){ .buffer_count = buffer_count, .swap_interval = swap_interval, .height = *height };
	const uint32_t pixels = *width * *height;
	for (uint32_t i = 0; i < buffer_count; i++) {
		present.buffers[i] = xsdk_gpu_alloc(pixels * 4);
		if (!present.buffers[i]) {
			for (uint32_t j = 0; j < i; j++)
				xsdk_gpu_free(present.buffers[j]);
			xsdk_display_close();
			return STATUS_NO_MEMORY;
		}
		for (uint32_t p = 0; p < pixels; p++)
			present.buffers[i][p] = BLACK;
		present.addresses[i] = xsdk_gpu_address(present.buffers[i]);
	}
	__asm__ volatile("sync" ::: "memory");
	// The last buffer stands for frame -1, so frame 0 resolves into the first.
	(void)xsdk_display_show(present.addresses[buffer_count - 1], *width);
	present.open = true;
	VdSetGraphicsInterruptCallback(on_interrupt, NULL);
	return STATUS_SUCCESS;
}

// Frame k reuses frame k - buffer_count's buffer, off screen once frame k - buffer_count + 1 has been shown.
NTSTATUS xsdk_present_begin(uint32_t *address, uint32_t timeout_ms)
{
	assert(present.open);
	const uint32_t frame = present.queued;
	const uint32_t needed = frame - present.buffer_count + 2;
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	while ((int32_t)(present.flipped - needed) < 0) {
		if (elapsed_us(&start) >= timeout_ms * 1000)
			return STATUS_IO_TIMEOUT;
		(void)xsdk_thread_sleep(1);
	}
	const uint32_t waited_us = elapsed_us(&start);
	present.longest_wait_us = waited_us > present.longest_wait_us ? waited_us : present.longest_wait_us;
	*address = present.addresses[frame % present.buffer_count];
	return STATUS_SUCCESS;
}

volatile uint32_t *xsdk_present_pixels(uint32_t address)
{
	for (uint32_t i = 0; i < present.buffer_count; i++) {
		if (present.addresses[i] == address)
			return present.buffers[i];
	}
	assert(false);
	return NULL;
}

void xsdk_present_end(void)
{
	assert(present.open);
	const uint32_t frame = present.queued;
	present.fences[frame % QUEUE_SLOTS] = xsdk_gpu_fence();
	// Publish the fence before the count the callback reads.
	__asm__ volatile("lwsync" ::: "memory");
	present.queued = frame + 1;
}

void xsdk_present_stats(XsdkPresentStats *stats)
{
	*stats = (XsdkPresentStats){ .vblanks = present.vblanks,
		.flips = present.flipped,
		.missed = present.missed,
		.late = present.late,
		.first_flip_vblank = present.first_flip_vblank,
		.last_flip_vblank = present.last_flip_vblank,
		.longest_wait_us = present.longest_wait_us };
}

void xsdk_present_close(void)
{
	if (!present.open)
		return;
	present.stopping = true;
	for (uint32_t waited_ms = 0; present.flipped != present.queued && waited_ms < CLOSE_TIMEOUT_MS; waited_ms++)
		(void)xsdk_thread_sleep(1);
	// So the kernel doesn't call into the title once it unloads; XAM runs again afterwards (docs/HARDWARE.md).
	VdSetGraphicsInterruptCallback(0, 0);
	xsdk_display_close();
	for (uint32_t i = 0; i < present.buffer_count; i++)
		xsdk_gpu_free(present.buffers[i]);
	present.open = false;
}
