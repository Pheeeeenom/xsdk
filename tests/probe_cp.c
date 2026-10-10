#include <stdint.h>

#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 4's first question (SCOPE.md section 3.2, option 1): does the command processor still hold microcode when a
// title starts, and can the title read it back? Logs checksums only, never the microcode itself.

enum {
	PFP_WORDS = 0x120,
	ME_WORDS = 0x900,
	// libxenon sets it before touching the microcode and checks it reads back set (drivers/xenos/xe.c:305-318).
	CP_HALTED = 0x10000000,
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); offsets are in bytes, as libxenon's.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

typedef struct {
	uint32_t crc;
	uint32_t nonzero_words;
} RamSummary;

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

// Reads a microcode RAM from word 0 through its address and data registers, as libxenon's loader does
// (libxenon:drivers/xenos/xe.c:228-247).
static RamSummary summarize_ram(uint32_t address_offset, uint32_t data_offset, uint32_t words)
{
	write_register(address_offset, 0);
	// libxenon waits 100 us after setting the PFP's address; the kernel's shortest sleep is longer.
	(void)xsdk_thread_sleep(1);
	uint32_t crc = 0xFFFFFFFF;
	uint32_t nonzero_words = 0;
	for (uint32_t i = 0; i < words; i++) {
		const uint32_t word = read_register(data_offset);
		nonzero_words += word != 0;
		crc = crc32_word(crc, word);
	}
	return (RamSummary){ .crc = ~crc, .nonzero_words = nonzero_words };
}

static void report_ram(const char *name, uint32_t address_offset, uint32_t data_offset, uint32_t words)
{
	xsdk_log_write("cp: reading %s RAM", name);
	const RamSummary first = summarize_ram(address_offset, data_offset, words);
	const RamSummary second = summarize_ram(address_offset, data_offset, words);
	xsdk_log_write("cp: %s RAM: %u words, %u non-zero; crc32 %08X, then %08X on a second read", name, (unsigned)words,
		(unsigned)first.nonzero_words, (unsigned)first.crc, (unsigned)second.crc);
}

// What each register means comes from how libxenon uses it (drivers/xenos/xe.c:104, 210-221, 258, 305-330, 389-395).
static void report_state(const char *when)
{
	xsdk_log_write("cp: %s: 0x07D8 %08X (bit 28: halted), 0x1740 %08X (bit 31: busy), 0x0E6C %08X", when,
		(unsigned)read_register(0x07D8), (unsigned)read_register(0x1740), (unsigned)read_register(0x0E6C));
}

int main(void)
{
	xsdk_log_write("cp probe");
	report_state("at start");
	xsdk_log_write("cp: ring: base %08X, control %08X, read pointer write-back %08X, read %08X, write %08X",
		(unsigned)read_register(0x0700), (unsigned)read_register(0x0704), (unsigned)read_register(0x070C),
		(unsigned)read_register(0x0710), (unsigned)read_register(0x0714));

	// Reading the PFP while it runs hung the GPU (docs/HARDWARE.md), so halt first, as libxenon does (xe.c:305-306).
	const uint32_t control = read_register(0x07D8);
	write_register(0x07D8, control | CP_HALTED);
	(void)xsdk_thread_sleep(2);
	report_state("halted");

	report_ram("PFP", 0x117C, 0x1180, PFP_WORDS);
	report_ram("ME", 0x07E4, 0x07E8, ME_WORDS);

	write_register(0x07D8, control);
	(void)xsdk_thread_sleep(2);
	report_state("resumed");
	return 0;
}
