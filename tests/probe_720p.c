#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Applies the kernel's packets for scaling 1280x720 to the output, shows a 1280x720 image, then puts registers back.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	SHOW_MS = 12000,
	DOUBLE_BUFFER_POLLS = 100000,
	MAX_COMMANDS = 512,
	MAX_WRITES = 256,
	CORNER = 32,
	BLOCK = 128,
	GREY = 0xFF404040,
	WHITE = 0xFFFFFFFF,
	BLACK = 0xFF000000,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	// The kernel's own filter type both ways (docs/HARDWARE.md, "The console outputs 1920 pixels wide").
	FILTER_TYPE = 1,
	// A type-2 packet, one dword that does nothing (xenia:src/xenia/gpu/command_processor.cc:677-683).
	TYPE2_NOP = 0x80000000,
	TYPE0_ONE_REGISTER = 1 << 15,
};

typedef struct {
	uint32_t index;
	uint32_t value;
	uint32_t saved;
} Write;

static uint32_t commands[MAX_COMMANDS];
static uint32_t unknown[MAX_COMMANDS];
static Write writes[MAX_WRITES];
static uint32_t write_count;

static void log_display(void)
{
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	xsdk_log_write("720p: VdQueryVideoMode: %ux%u, interlaced %u", (unsigned)mode.display_width,
		(unsigned)mode.display_height, (unsigned)mode.is_interlaced);
}

// The display reads a word's bytes as B, G, R, A (docs/HARDWARE.md, "A title's buffer on screen").
static void put(volatile uint32_t *pixels, uint32_t x, uint32_t y, uint32_t argb)
{
	pixels[tiled_index(x, y, WIDTH)] = __builtin_bswap32(argb);
}

static void fill(volatile uint32_t *pixels, uint32_t x0, uint32_t y0, uint32_t size, uint32_t argb)
{
	for (uint32_t y = y0; y < y0 + size; y++)
		for (uint32_t x = x0; x < x0 + size; x++)
			put(pixels, x, y, argb);
}

// A one-pixel white border, a square in each corner, and blocks of one-pixel stripes and checks in the middle.
static void draw_image(volatile uint32_t *pixels)
{
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			put(pixels, x, y, x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 ? WHITE : GREY);
	fill(pixels, 1, 1, CORNER, RED);
	fill(pixels, WIDTH - 1 - CORNER, 1, CORNER, GREEN);
	fill(pixels, 1, HEIGHT - 1 - CORNER, CORNER, BLUE);
	fill(pixels, WIDTH - 1 - CORNER, HEIGHT - 1 - CORNER, CORNER, YELLOW);
	const uint32_t top = HEIGHT / 2 - BLOCK / 2;
	for (uint32_t y = 0; y < BLOCK; y++) {
		for (uint32_t x = 0; x < BLOCK; x++) {
			put(pixels, WIDTH / 2 - BLOCK * 3 / 2 + x, top + y, x & 1 ? WHITE : BLACK);
			put(pixels, WIDTH / 2 - BLOCK / 2 + x, top + y, y & 1 ? WHITE : BLACK);
			put(pixels, WIDTH / 2 + BLOCK / 2 + x, top + y, (x ^ y) & 1 ? WHITE : BLACK);
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

static void record_write(uint32_t index, uint32_t value)
{
	if (write_count < MAX_WRITES)
		writes[write_count++] = (Write){ .index = index, .value = value, .saved = probe_gpu_registers[index] };
}

// Logs every packet, keeping the register writes of type-0 packets (xenia:src/xenia/gpu/command_processor.cc:618-683).
static void decode(uint32_t count)
{
	for (uint32_t i = 0; i < count && i < MAX_COMMANDS;) {
		const uint32_t header = commands[i];
		const uint32_t type = header >> 30;
		if (type == 0) {
			const uint32_t values = (header >> 16 & 0x3FFF) + 1;
			const uint32_t index = header & 0x7FFF;
			const bool one_register = header & TYPE0_ONE_REGISTER;
			for (uint32_t v = 0; v < values && i + 1 + v < count; v++) {
				const uint32_t target = one_register ? index : index + v;
				xsdk_log_write("720p: dword %u: write %04X (byte %04X) = %08X", (unsigned)i, (unsigned)target,
					(unsigned)(target * 4), (unsigned)commands[i + 1 + v]);
				record_write(target, commands[i + 1 + v]);
			}
			i += 1 + values;
		} else if (type == 2) {
			i++;
		} else {
			const uint32_t payload = type == 3 ? (header >> 16 & 0x3FFF) + 1 : 2;
			xsdk_log_write("720p: dword %u: type %u packet %08X, %u dwords after it, not applied", (unsigned)i,
				(unsigned)type, (unsigned)header, (unsigned)payload);
			for (uint32_t v = 0; v < payload && i + 1 + v < count; v++)
				xsdk_log_write("720p:   %08X", (unsigned)commands[i + 1 + v]);
			i += 1 + payload;
		}
	}
}

// xsdk_display_show's writes for a WIDTH by HEIGHT surface, then 0x60EC.
static void show(uint32_t address)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[UNKNOWN_0A10] = address;
	r[UNKNOWN_0A11] = WIDTH;
	r[D1GRPH_PRIMARY_SURFACE_ADDRESS] = address;
	r[D1GRPH_PITCH] = WIDTH;
	r[D1GRPH_X_END] = WIDTH;
	r[D1GRPH_Y_END] = HEIGHT;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	for (uint32_t polls = 0; r[D1CRTC_DOUBLE_BUFFER_CONTROL] && polls < DOUBLE_BUFFER_POLLS; polls++)
		;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
}

int main(void)
{
	xsdk_log_write("720p kernel scaler probe");
	const NTSTATUS status = xsdk_gpu_open();
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = NT_SUCCESS(status) ? xsdk_display_open(&width, &height) : status;
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	xsdk_log_write("720p: GPU 0x%08X, display 0x%08X %ux%u", (unsigned)status, (unsigned)display_status,
		(unsigned)width, (unsigned)height);
	if (!NT_SUCCESS(display_status) || !pixels)
		return 1;
	log_display();
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	if (!mode.display_width || !mode.display_height)
		return 1;
	draw_image(pixels);

	const D3DFILTER_PARAMETERS filter = { .nyquist = 0.0f, .flicker_filter = 0.0f, .beta = 0.0f };
	for (uint32_t i = 0; i < MAX_COMMANDS; i++)
		commands[i] = TYPE2_NOP;
	const uint32_t count =
		VdInitializeScalerCommandBuffer(0, HEIGHT << 16 | WIDTH, 0, mode.display_height << 16 | mode.display_width,
			HEIGHT << 16 | WIDTH, FILTER_TYPE, &filter, FILTER_TYPE, &filter, unknown, commands, MAX_COMMANDS);
	xsdk_log_write("720p: VdInitializeScalerCommandBuffer(1280x720 to %ux%u) returned %u", (unsigned)mode.display_width,
		(unsigned)mode.display_height, (unsigned)count);
	decode(count < MAX_COMMANDS ? count : MAX_COMMANDS);
	xsdk_log_write("720p: %u register writes", (unsigned)write_count);

	for (uint32_t i = 0; i < write_count; i++)
		probe_gpu_registers[writes[i].index] = writes[i].value;
	show(xsdk_gpu_address(pixels));
	xsdk_log_write("720p: the kernel's writes applied and the 1280x720 image shown; %u ms", (unsigned)SHOW_MS);
	(void)xsdk_thread_sleep(SHOW_MS);

	for (uint32_t i = write_count; i > 0; i--)
		probe_gpu_registers[writes[i - 1].index] = writes[i - 1].saved;
	xsdk_display_close();
	xsdk_gpu_free(pixels);
	xsdk_log_write("720p: registers put back; the GPU goes back at exit");
	return 0;
}
