#ifndef XSDK_PROBE_GPU_H
#define XSDK_PROBE_GPU_H

#include <stdint.h>
#include <time.h>

#include "xsdk/log.h"

// Command processor registers by index.
enum {
	// Named by D3D's GPU hang report (docs/HARDWARE.md).
	CP_RB_BASE = 0x01C0,
	CP_RB_RPTR = 0x01C4,
	RBBM_STATUS = 0x05D0,
	// Named by Xenia (xenia:src/xenia/gpu/command_processor.cc:317-320, graphics_system.cc:211).
	CP_RB_CNTL = 0x01C1,
	CP_RB_RPTR_ADDR = 0x01C3,
	CP_RB_WPTR = 0x01C5,
	// Bit 28 halts the command processor (libxenon:drivers/xenos/xe.c:305-318).
	UNKNOWN_01F6 = 0x01F6,
	// RBBM_STATUS bit 31 (libxenon:drivers/xenos/xe.c:258).
	GPU_BUSY = 0x80000000,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-54), else Xenia's style.
enum {
	UNKNOWN_0A10 = 0x0A10,
	UNKNOWN_0A11 = 0x0A11,
	D1CRTC_DOUBLE_BUFFER_CONTROL = 0x183B,
	D1GRPH_ENABLE = 0x1840,
	D1GRPH_CONTROL = 0x1841,
	D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x1844,
	D1GRPH_PITCH = 0x1848,
	D1GRPH_X_END = 0x184D,
	D1GRPH_Y_END = 0x184E,
	D1GRPH_UPDATE = 0x1851,
	D1GRPH_FLIP_CONTROL = 0x1852,
	DC_LUT_RW_MODE = 0x1921,
	DC_LUT_RW_INDEX = 0x1922,
	DC_LUT_WRITE_EN_MASK = 0x1927,
	DC_LUT_AUTOFILL = 0x1928,
	DC_LUTA_CONTROL = 0x1930,
	AVIVO_D1MODE_VIEWPORT_SIZE = 0x1961,
	AVIVO_D1SCL_SCALER_ENABLE = 0x1964,
};

// GPU registers as titles see them, one dword each (xenia:src/xenia/gpu/graphics_system.cc:92-95).
static volatile uint32_t *const probe_gpu_registers = (volatile uint32_t *)0x7FC80000;

static inline uint32_t probe_elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

// Logs what D3D's GPU hang report shows, plus the halt control (docs/HARDWARE.md); the log names registers in bytes.
static inline void probe_report_cp(const char *probe, const char *when)
{
	volatile const uint32_t *const registers = probe_gpu_registers;
	xsdk_log_write("%s: %s: 0x07D8 %08X (bit 28: halted), 0x1740 %08X (bit 31: busy)", probe, when,
		(unsigned)registers[UNKNOWN_01F6], (unsigned)registers[RBBM_STATUS]);
	xsdk_log_write("%s: %s: ring base %08X, control %08X, read pointer write-back %08X, read %08X, write %08X", probe,
		when, (unsigned)registers[CP_RB_BASE], (unsigned)registers[CP_RB_CNTL], (unsigned)registers[CP_RB_RPTR_ADDR],
		(unsigned)registers[CP_RB_RPTR], (unsigned)registers[CP_RB_WPTR]);
}

#endif
