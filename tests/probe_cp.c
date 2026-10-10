#include <stdint.h>

#include "xsdk/log.h"

// Logs the command processor's state as a title finds it, and a checksum of its ME microcode, never the microcode.
// Don't touch the PFP microcode RAM (0x117C, 0x1180): it doesn't read back and it hangs the GPU (docs/HARDWARE.md).

enum { ME_WORDS = 0x900 };

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); offsets are in bytes, as libxenon's.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static void write_register(uint32_t offset, uint32_t value)
{
	gpu_registers[offset / 4] = value;
}

// CRC-32 over each word's big-endian bytes, so a host can compare it with zlib.crc32.
static uint32_t crc32_word(uint32_t crc, uint32_t word)
{
	for (int shift = 24; shift >= 0; shift -= 8) {
		crc ^= (word >> shift) & 0xFF;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
	}
	return crc;
}

// Reads the ME microcode RAM from word 0, as libxenon's loader checks it (libxenon:drivers/xenos/xe.c:243-247).
static void report_me_ram(void)
{
	write_register(0x07E4, 0);
	uint32_t crc = 0xFFFFFFFF;
	uint32_t nonzero_words = 0;
	for (uint32_t i = 0; i < ME_WORDS; i++) {
		const uint32_t word = read_register(0x07E8);
		nonzero_words += word != 0;
		crc = crc32_word(crc, word);
	}
	xsdk_log_write(
		"cp: ME RAM: %u words, %u non-zero, crc32 %08X", (unsigned)ME_WORDS, (unsigned)nonzero_words, (unsigned)~crc);
}

// What each register means comes from how libxenon uses it (drivers/xenos/xe.c:104, 210-221, 258, 305-330, 389-395).
static void report_state(void)
{
	xsdk_log_write("cp: 0x07D8 %08X (bit 28: halted), 0x1740 %08X (bit 31: busy), 0x0E6C %08X",
		(unsigned)read_register(0x07D8), (unsigned)read_register(0x1740), (unsigned)read_register(0x0E6C));
	xsdk_log_write("cp: ring: base %08X, control %08X, read pointer write-back %08X, read %08X, write %08X",
		(unsigned)read_register(0x0700), (unsigned)read_register(0x0704), (unsigned)read_register(0x070C),
		(unsigned)read_register(0x0710), (unsigned)read_register(0x0714));
}

int main(void)
{
	xsdk_log_write("cp probe");
	report_state();
	report_me_ram();
	return 0;
}
