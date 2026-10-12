#include <stdint.h>
#include <string.h>

#include "audio_frame.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// A title for listening: each step plays a tone on known channels, so a person can confirm the frame layout.

enum {
	SPLASH_MS = 2000,
	STEP_MS = 2000,
	GAP_MS = 1000,
};

typedef struct {
	const char *description;
	uint32_t channels;
	int frequency_hz;
} Step;

static const Step steps[] = {
	{ "front left only, 375 Hz", 1u << CHANNEL_FRONT_LEFT, 375 },
	{ "front right only, 375 Hz", 1u << CHANNEL_FRONT_RIGHT, 375 },
	{ "centre only, 375 Hz (a stereo TV may play it from both sides, or not at all)", 1u << CHANNEL_CENTER, 375 },
	{ "back left only, 375 Hz (a stereo TV may play it from the left)", 1u << CHANNEL_BACK_LEFT, 375 },
	{ "back right only, 375 Hz (a stereo TV may play it from the right)", 1u << CHANNEL_BACK_RIGHT, 375 },
	{ "both front channels, 750 Hz: one octave above every tone before it",
		1u << CHANNEL_FRONT_LEFT | 1u << CHANNEL_FRONT_RIGHT, 750 },
};

enum { STEP_COUNT = sizeof(steps) / sizeof(steps[0]) };

void audio_check_callback(const uint32_t *context);

static uint32_t driver;
static const float *playing;
static uint32_t callbacks;

// Runs on the kernel's audio thread each time the driver wants another frame.
void audio_check_callback(const uint32_t *context)
{
	(void)context;
	const uint32_t current = __atomic_load_n(&driver, __ATOMIC_ACQUIRE);
	// The first callbacks can arrive before registration has stored the driver.
	if (!current)
		return;
	(void)XAudioSubmitRenderDriverFrame(current, __atomic_load_n(&playing, __ATOMIC_ACQUIRE));
	__atomic_fetch_add(&callbacks, 1, __ATOMIC_RELAXED);
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	// Frame 0 is silence; frame i + 1 belongs to steps[i].
	float *frames = MmAllocatePhysicalMemoryEx(0, FRAME_BYTES * (STEP_COUNT + 1), PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!frames) {
		xsdk_log_write("audio check: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	memset(frames, 0, FRAME_BYTES);
	for (int i = 0; i < STEP_COUNT; i++)
		audio_frame_fill(frames + (i + 1) * FRAME_FLOATS, steps[i].channels, steps[i].frequency_hz);
	__atomic_store_n(&playing, frames, __ATOMIC_RELEASE);

	const XAUDIO_RENDER_DRIVER_CLIENT client = { .callback = audio_check_callback, .context = 0 };
	uint32_t registered = 0;
	const uint32_t result = XAudioRegisterRenderDriverClient(&client, &registered);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("audio check: XAudioRegisterRenderDriverClient failed: 0x%08X", (unsigned)result);
		return 1;
	}
	__atomic_store_n(&driver, registered, __ATOMIC_RELEASE);

	xsdk_log_write("audio check: %d steps of %d ms, with %d ms of silence before each", STEP_COUNT, STEP_MS, GAP_MS);
	for (int i = 0; i < STEP_COUNT; i++) {
		(void)xsdk_thread_sleep(GAP_MS);
		xsdk_log_write("audio check: step %d: %s", i + 1, steps[i].description);
		__atomic_store_n(&playing, frames + (i + 1) * FRAME_FLOATS, __ATOMIC_RELEASE);
		(void)xsdk_thread_sleep(STEP_MS);
		__atomic_store_n(&playing, frames, __ATOMIC_RELEASE);
	}
	(void)xsdk_thread_sleep(GAP_MS);

	__atomic_store_n(&driver, 0, __ATOMIC_RELEASE);
	(void)XAudioUnregisterRenderDriverClient(registered);
	xsdk_log_write("audio check: done, %u frames submitted", (unsigned)__atomic_load_n(&callbacks, __ATOMIC_RELAXED));
	return 0;
}
