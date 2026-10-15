#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "bar_scene.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"

// Milestone 37's check, watched on the console: the moving bar while the user opens the Guide and closes it, twice,
// then presses Back. The Guide shows over the bar, the bar moving on under it, and the bar runs on whole after it
// closes (docs/scope/37-guide.md). The title runs until Back, which XAM keeps from it while the Guide is open, or
// until the user leaves through the Guide.

enum {
	SPLASH_MS = 2000,
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	BAR_ARGB = 0xFF00FFFF,
	// The Guide's first opening held a frame up 2.7 s while XAM loaded it (docs/HARDWARE.md).
	BEGIN_TIMEOUT_MS = 5000,
	MAX_WRONG_LOGGED = 4,
	GUIDE_OPENINGS = 2,
	// XN_SYS_UI, its parameter nonzero while the Guide shows (xenia:src/xenia/kernel/xam/xam_ui.cc:72-75); every
	// area's notifications, as the console gave the probes (docs/HARDWARE.md), and Xenia's highest version
	// (xenia:src/xenia/kernel/xam/xam_notify.cc:21-28).
	XN_SYS_UI = 9,
	NOTIFY_MAX_VERSION = 10,
};

static uint32_t passed_count;
static uint32_t failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

static void log_state(const char *what, uint32_t ms, uint32_t frame)
{
	XsdkPresentStats present;
	xsdk_present_stats(&present);
	XsdkGpuStats gpu;
	xsdk_gpu_stats(&gpu);
	xsdk_log_write(
		"guide: %s at %u ms, frame %u: XAM drew %u frames, its surface showed %u; %u flips, %u missed, longest wait "
		"%u us",
		what, (unsigned)ms, (unsigned)frame, (unsigned)gpu.system_draws, (unsigned)present.system_frames,
		(unsigned)present.flips, (unsigned)present.missed, (unsigned)present.longest_wait_us);
}

int main(void)
{
	xsdk_log_write("guide: open the Guide and close it, twice, then press Back");
	(void)xsdk_splash_show(SPLASH_MS);
	const NTSTATUS status = xsdk_gpu_open();
	BarScene scene;
	const bool opened = NT_SUCCESS(status) && bar_scene_open(&scene, BAR_ARGB);
	uint32_t width = 0;
	uint32_t height = 0;
	const bool presenting =
		opened && NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height));
	const HANDLE listener = XamNotifyCreateListener(UINT64_MAX, NOTIFY_MAX_VERSION);
	xsdk_log_write("guide: GPU 0x%08X, display %ux%u, listener %08X", (unsigned)status, (unsigned)width,
		(unsigned)height, (unsigned)listener);
	check("guide: GPU, scene and present open, and a listener", presenting && listener);
	if (!presenting || !listener)
		return 1;

	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t frame = 0;
	uint32_t wrong = 0;
	uint32_t openings = 0;
	uint32_t closings = 0;
	uint32_t system_frames = 0;
	uint32_t last_system_frame = 0;
	// The frames the Guide last opened and closed at.
	uint32_t opened_frame = 0;
	uint32_t closed_frame = 0;
	bool kept_up = true;
	for (;;) {
		INPUT_STATE input;
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			break;
		uint32_t address;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
			kept_up = false;
			break;
		}
		// The buffer last held frame - BUFFERS; XAM draws in a surface of its own and leaves the title's alone.
		if (frame > BUFFERS && !bar_scene_holds(&scene, xsdk_present_pixels(address), frame - BUFFERS)) {
			if (wrong < MAX_WRONG_LOGGED)
				bar_scene_log_miss(&scene, xsdk_present_pixels(address), frame - BUFFERS, "guide");
			wrong++;
		}
		bar_scene_draw(&scene, frame, address);
		xsdk_present_end();
		XsdkPresentStats stats;
		xsdk_present_stats(&stats);
		if (stats.system_frames != system_frames) {
			system_frames = stats.system_frames;
			last_system_frame = frame;
		}
		uint32_t id = 0;
		uint32_t param = 0;
		while (XNotifyGetNext(listener, 0, &id, &param)) {
			if (id != XN_SYS_UI)
				continue;
			openings += param != 0;
			closings += param == 0;
			if (param)
				opened_frame = frame;
			else if (openings)
				closed_frame = frame;
			log_state(param ? "the Guide opened" : "the Guide closed", elapsed_ms(&start), frame);
		}
		frame++;
	}
	log_state("done", elapsed_ms(&start), frame);
	xsdk_present_close();
	(void)NtClose(listener);
	XsdkGpuStats gpu;
	xsdk_gpu_stats(&gpu);
	check("guide: every frame queued in time", kept_up);
	check("guide: the Guide opened and closed twice", openings >= GUIDE_OPENINGS && closings >= openings);
	check("guide: XAM drew the Guide over frames", gpu.system_draws > 0);
	check("guide: XAM's surface showed in their place", system_frames > 0);
	// XAM's surface last showed no later than the frame the Guide closed at, and the title's own frames came after it.
	check("guide: the title's own frames showed again after it closed",
		system_frames > 0 && closed_frame > opened_frame && last_system_frame <= closed_frame &&
			frame > closed_frame + 1);
	check("guide: every frame resolved with its bar", wrong == 0);
	bar_scene_close(&scene);
	check("guide: GPU close", NT_SUCCESS(xsdk_gpu_close()));
	xsdk_log_write("guide: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
	return failed_count != 0;
}
