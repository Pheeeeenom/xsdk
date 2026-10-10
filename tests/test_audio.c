#include <stdint.h>

#include "audio_frame.h"
#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	TONE_HZ = 375,
	TONE_MS = 2000,
};

void test_audio_entry(const uint32_t *context);
void test_audio_callback(const uint32_t *context);

static float *frame;
static uint32_t driver;
static uint32_t callbacks;

// Runs on the kernel's audio thread each time the driver wants another frame.
void test_audio_callback(const uint32_t *context)
{
	(void)context;
	const uint32_t current = __atomic_load_n(&driver, __ATOMIC_ACQUIRE);
	// The first callbacks can arrive before registration has stored the driver.
	if (!current)
		return;
	(void)XAudioSubmitRenderDriverFrame(current, frame);
	__atomic_fetch_add(&callbacks, 1, __ATOMIC_RELAXED);
}

void test_audio(void)
{
	frame = MmAllocatePhysicalMemoryEx(0, FRAME_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!frame) {
		xsdk_log_write("audio: MmAllocatePhysicalMemoryEx failed");
		check("audio tone", false);
		return;
	}
	audio_frame_fill(frame, 1u << CHANNEL_FRONT_LEFT | 1u << CHANNEL_FRONT_RIGHT, TONE_HZ);

	const XAUDIO_RENDER_DRIVER_CLIENT client = { .callback = test_audio_entry, .context = 0 };
	uint32_t registered = 0;
	const uint32_t result = XAudioRegisterRenderDriverClient(&client, &registered);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("audio: XAudioRegisterRenderDriverClient failed: 0x%08X", (unsigned)result);
		check("audio tone", false);
		return;
	}
	__atomic_store_n(&driver, registered, __ATOMIC_RELEASE);
	xsdk_log_write("audio: playing a %d Hz tone for %d ms", TONE_HZ, TONE_MS);
	(void)xsdk_thread_sleep(TONE_MS);
	__atomic_store_n(&driver, 0, __ATOMIC_RELEASE);
	(void)XAudioUnregisterRenderDriverClient(registered);

	const uint32_t count = __atomic_load_n(&callbacks, __ATOMIC_RELAXED);
	xsdk_log_write("audio: %u frames submitted from callbacks", (unsigned)count);
	check("audio tone", count > 0);
}
