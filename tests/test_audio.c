#include <math.h>
#include <stdint.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Frames hold each channel's big-endian floats one channel after another (xenia:src/xenia/apu/conversion.h:86-95).
enum {
	FRAME_CHANNELS = 6,     // xenia:src/xenia/apu/xaudio2/xaudio2_audio_driver.h:98
	CHANNEL_SAMPLES = 256,  // xenia:src/xenia/apu/xaudio2/xaudio2_audio_driver.h:99
	SAMPLE_RATE_HZ = 48000, // xenia:src/xenia/apu/xaudio2/xaudio2_audio_driver.cc:144
	FRAME_BYTES = FRAME_CHANNELS * CHANNEL_SAMPLES * sizeof(float),
	// Two whole periods fit in a frame, so one frame can repeat without a click.
	TONE_HZ = 375,
	TONE_PERIOD_SAMPLES = SAMPLE_RATE_HZ / TONE_HZ,
	TONE_MS = 2000,
};

static const float TONE_AMPLITUDE = 0.25f;

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

static void fill_frame(void)
{
	for (int sample = 0; sample < CHANNEL_SAMPLES; sample++) {
		const float value = TONE_AMPLITUDE * sinf(2.0f * (float)M_PI * (float)sample / TONE_PERIOD_SAMPLES);
		for (int channel = 0; channel < FRAME_CHANNELS; channel++) {
			// Front left and front right only.
			frame[channel * CHANNEL_SAMPLES + sample] = channel < 2 ? value : 0.0f;
		}
	}
}

void test_audio(void)
{
	frame = MmAllocatePhysicalMemoryEx(0, FRAME_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!frame) {
		xsdk_log_write("audio: MmAllocatePhysicalMemoryEx failed");
		check("audio tone", false);
		return;
	}
	fill_frame();

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
