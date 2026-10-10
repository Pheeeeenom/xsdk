#include <stdint.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	POLL_COUNT = 30,
	POLL_INTERVAL_MS = 100,
};

static void log_state(const INPUT_STATE *state)
{
	const INPUT_GAMEPAD *pad = &state->gamepad;
	xsdk_log_write("input: packet %u buttons 0x%04X triggers %u %u left %d %d right %d %d",
		(unsigned)state->packet_number, pad->buttons, pad->left_trigger, pad->right_trigger, pad->thumb_lx,
		pad->thumb_ly, pad->thumb_rx, pad->thumb_ry);
}

void test_input(void)
{
	xsdk_log_write("input: polling controller 1 for 3 seconds; press some buttons");
	uint32_t result = ERROR_DEVICE_NOT_CONNECTED;
	uint32_t last_packet = UINT32_MAX;
	int reads = 0;
	for (int i = 0; i < POLL_COUNT; i++) {
		INPUT_STATE state;
		result = XamInputGetState(0, 0, &state);
		if (result == ERROR_SUCCESS) {
			reads++;
			if (state.packet_number != last_packet)
				log_state(&state);
			last_packet = state.packet_number;
		}
		(void)xsdk_thread_sleep(POLL_INTERVAL_MS);
	}
	if (!reads)
		xsdk_log_write("input: XamInputGetState returned 0x%X", (unsigned)result);
	check("controller state read", reads > 0);
}
