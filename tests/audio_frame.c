#include "audio_frame.h"

#include <math.h>

static const float TONE_AMPLITUDE = 0.25f;

void audio_frame_fill(float *frame, uint32_t channel_mask, int frequency_hz)
{
	for (int sample = 0; sample < CHANNEL_SAMPLES; sample++) {
		const float phase = 2.0f * (float)M_PI * (float)frequency_hz * (float)sample / SAMPLE_RATE_HZ;
		const float value = TONE_AMPLITUDE * sinf(phase);
		for (int channel = 0; channel < FRAME_CHANNELS; channel++)
			frame[channel * CHANNEL_SAMPLES + sample] = channel_mask & (1u << channel) ? value : 0.0f;
	}
}
