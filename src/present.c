#include "xsdk/present.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "gpu_private.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
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

// What a queued frame shows: its front buffer, or XAM's surface while the Guide is open (docs/HARDWARE.md, "The Guide
// shows through VdSwap").
typedef struct {
	uint32_t address;
	// Its size and the scaler for it, where the display can change them.
	DisplaySize size;
	bool system;
} Shown;

typedef struct {
	volatile uint32_t *buffers[XSDK_PRESENT_MAX_BUFFERS];
	uint32_t addresses[XSDK_PRESENT_MAX_BUFFERS];
	uint32_t buffer_count;
	uint32_t swap_interval;
	uint32_t width;
	uint32_t height;
	// The front buffers' own size, where the display can also show XAM's surface in their place.
	DisplaySize own_size;
	bool sized;
	bool unshown_logged;
	uint32_t longest_wait_us;
	bool open;
	// The splash's logo shows until the first flip, and its buffers wait for one more vblank after that.
	bool splash_held;
	// Written by the title.
	volatile uint32_t queued;
	volatile uint32_t fences[QUEUE_SLOTS];
	Shown shown[QUEUE_SLOTS];
	volatile uint32_t system_frames;
	volatile bool stopping;
	// Written by the callback.
	volatile uint32_t flipped;
	volatile uint32_t vblanks;
	volatile uint32_t missed;
	volatile uint32_t late;
	volatile uint32_t first_late_vblank;
	volatile uint32_t first_late_line;
	volatile uint32_t first_flip_vblank;
	volatile uint32_t last_flip_vblank;
	bool showed_system;
} Present;

static Present present;

// What D3D tells XAM about the title's display, 64 bytes, which XAM draws the frame under the Guide by: the front
// buffer's size and the output's, then the scaler between them, the front buffer's corners, the output's size and the
// filters, 0 for the defaults (docs/HARDWARE.md, "The Guide shows through VdSwap").
typedef struct {
	uint16_t front_width;
	uint16_t front_height;
	uint16_t output_width;
	uint16_t output_height;
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;
	uint32_t scaled_width;
	uint32_t scaled_height;
	uint32_t filters[8];
} DisplayDescription;

_Static_assert(sizeof(DisplayDescription) == 64, "XAM copies 64 bytes");

enum {
	// VdCallGraphicsNotificationRoutines's event for a DisplayDescription.
	EVENT_DISPLAY = 1,
};

// Without it XAM keeps whatever display it was told of before, and draws that much of the frame: 640x480 of 1280x720
// on the console.
static void describe_display(uint32_t width, uint32_t height)
{
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	if (!xsdk_display_output(&output_width, &output_height))
		return;
	const DisplayDescription description = {
		.front_width = (uint16_t)width,
		.front_height = (uint16_t)height,
		.output_width = (uint16_t)output_width,
		.output_height = (uint16_t)output_height,
		.right = width,
		.bottom = height,
		.scaled_width = output_width,
		.scaled_height = output_height,
	};
	VdCallGraphicsNotificationRoutines(EVENT_DISPLAY, &description);
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
	const Shown *const shown = &present.shown[next % QUEUE_SLOTS];
	if (present.sized)
		xsdk_display_flip_sized(shown->address, &shown->size);
	else
		xsdk_display_flip(shown->address);
	// From the mode's last line on, the display is in vblank; XAM's surface counts lines of its own.
	const uint32_t line = gpu_registers[R500_D1MODE_V_COUNTER];
	const bool own = !shown->system && !present.showed_system;
	present.showed_system = shown->system;
	if (own && line < present.height) {
		if (!present.late) {
			present.first_late_vblank = vblank;
			present.first_late_line = line;
		}
		present.late = present.late + 1;
	}
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

NTSTATUS xsdk_present_open(
	XsdkDisplayMode mode, uint32_t buffer_count, uint32_t swap_interval, uint32_t *width, uint32_t *height)
{
	assert(!present.open && buffer_count >= 2 && buffer_count <= XSDK_PRESENT_MAX_BUFFERS);
	assert(swap_interval >= 1 && swap_interval <= 2);
	const bool splash_held = mode == XSDK_DISPLAY_720P && xsdk_splash_hand_over(width, height);
	const NTSTATUS status = splash_held ? STATUS_SUCCESS : xsdk_display_open(mode, width, height);
	if (!NT_SUCCESS(status))
		return status;
	present = (Present){ .buffer_count = buffer_count,
		.swap_interval = swap_interval,
		.width = *width,
		.height = *height,
		.splash_held = splash_held };
	present.sized = xsdk_display_plan(*width, *height, *width, &present.own_size);
	describe_display(*width, *height);
	const uint32_t pixels = xsdk_gpu_resolve_bytes(*width, *height) / 4;
	for (uint32_t i = 0; i < buffer_count; i++) {
		present.buffers[i] = xsdk_gpu_alloc(pixels * 4);
		if (!present.buffers[i]) {
			for (uint32_t j = 0; j < i; j++)
				xsdk_gpu_free(present.buffers[j]);
			xsdk_display_close();
			if (splash_held)
				xsdk_splash_free();
			return STATUS_NO_MEMORY;
		}
		for (uint32_t p = 0; p < pixels; p++)
			present.buffers[i][p] = BLACK;
		present.addresses[i] = xsdk_gpu_address(present.buffers[i]);
	}
	__asm__ volatile("sync" ::: "memory");
	// The last buffer stands for frame -1, so frame 0 resolves into the first; the splash's logo stands in for it.
	if (!splash_held)
		(void)xsdk_display_show(present.addresses[buffer_count - 1], *width);
	present.open = true;
	VdSetGraphicsInterruptCallback(on_interrupt, NULL);
	return STATUS_SUCCESS;
}

// Frees the splash's buffers once a vblank has passed since the first flip took the logo off screen.
static void release_splash(void)
{
	if (!present.splash_held || !present.flipped)
		return;
	__asm__ volatile("lwsync" ::: "memory");
	if (present.vblanks == present.first_flip_vblank)
		return;
	xsdk_splash_free();
	present.splash_held = false;
}

// Frame k reuses frame k - buffer_count's buffer, off screen once frame k - buffer_count + 1 has been shown.
NTSTATUS xsdk_present_begin(uint32_t *address, uint32_t timeout_ms)
{
	assert(present.open);
	release_splash();
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

uint32_t xsdk_present_buffer_address(uint32_t index)
{
	assert(present.open && index < present.buffer_count);
	return present.addresses[index];
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
	const uint32_t index = frame % present.buffer_count;
	Shown *const shown = &present.shown[frame % QUEUE_SLOTS];
	*shown = (Shown){ .address = present.addresses[index], .size = present.own_size };
	GpuSurface system;
	if (xsdk_gpu_swap(present.buffers[index], present.width, present.height, &system)) {
		if (present.sized) {
			shown->address = system.address;
			shown->system = true;
			(void)xsdk_display_plan(system.width, system.height, system.pitch, &shown->size);
			present.system_frames = present.system_frames + 1;
		} else if (!present.unshown_logged) {
			present.unshown_logged = true;
			xsdk_log_write("present: XAM's %ux%u surface can't show on the kernel's display mode, so the Guide doesn't",
				(unsigned)system.width, (unsigned)system.height);
		}
	}
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
		.first_late_vblank = present.first_late_vblank,
		.first_late_line = present.first_late_line,
		.first_flip_vblank = present.first_flip_vblank,
		.last_flip_vblank = present.last_flip_vblank,
		.longest_wait_us = present.longest_wait_us,
		.system_frames = present.system_frames };
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
	if (present.splash_held)
		xsdk_splash_free();
	for (uint32_t i = 0; i < present.buffer_count; i++)
		xsdk_gpu_free(present.buffers[i]);
	present.open = false;
}
