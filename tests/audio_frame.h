#ifndef XSDK_AUDIO_FRAME_H
#define XSDK_AUDIO_FRAME_H

#include <stdint.h>

// Frames hold each channel's big-endian floats one channel after another (xenia:src/xenia/apu/conversion.h:86-95).
enum {
	FRAME_CHANNELS = 6,     // xenia:src/xenia/apu/xaudio2/xaudio2_audio_driver.h:98
	CHANNEL_SAMPLES = 256,  // xenia:src/xenia/apu/xaudio2/xaudio2_audio_driver.h:99
	SAMPLE_RATE_HZ = 48000, // xenia:src/xenia/apu/xaudio2/xaudio2_audio_driver.cc:144
	FRAME_FLOATS = FRAME_CHANNELS * CHANNEL_SAMPLES,
	FRAME_BYTES = FRAME_FLOATS * sizeof(float),
};

// The 5.1 channel order (xenia:src/xenia/apu/conversion.h:99).
enum {
	CHANNEL_FRONT_LEFT,
	CHANNEL_FRONT_RIGHT,
	CHANNEL_CENTER,
	CHANNEL_LOW_FREQUENCY,
	CHANNEL_BACK_LEFT,
	CHANNEL_BACK_RIGHT,
};

// A sine on each channel in channel_mask; frequency_hz must fit whole periods in a frame (187.5 Hz times 2^n).
void audio_frame_fill(float *frame, uint32_t channel_mask, int frequency_hz);

#endif
