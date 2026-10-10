#ifndef XSDK_PROBE_GPU_H
#define XSDK_PROBE_GPU_H

#include <assert.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/gpu.h"
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

// Command, shader-unit and drawing registers by index (xenia:src/xenia/gpu/register_table.inc), else Xenia's style.
enum {
	UNKNOWN_0081 = 0x0081,
	UNKNOWN_0082 = 0x0082,
	SCRATCH_UMSK = 0x01DC,
	SCRATCH_ADDR = 0x01DD,
	SCRATCH_REG0 = 0x0578,
	WAIT_UNTIL = 0x05C8,
	UNKNOWN_0A02 = 0x0A02,
	UNKNOWN_0A03 = 0x0A03,
	UNKNOWN_0A04 = 0x0A04,
	UNKNOWN_0A05 = 0x0A05,
	COHER_SIZE_HOST = 0x0A2F,
	COHER_BASE_HOST = 0x0A30,
	COHER_STATUS_HOST = 0x0A31,
	PA_CL_ENHANCE = 0x0C85,
	SQ_GPR_MANAGEMENT = 0x0D00,
	SQ_FLOW_CONTROL = 0x0D01,
	SQ_INST_STORE_MANAGMENT = 0x0D02,
	SQ_EO_RT = 0x0D04,
	UNKNOWN_0E42 = 0x0E42,
	RB_BC_CONTROL = 0x0F01,
	RB_SURFACE_INFO = 0x2000,
	RB_COLOR_INFO = 0x2001,
	RB_DEPTH_INFO = 0x2002,
	COHER_DEST_BASE_1 = 0x2007,
	COHER_DEST_BASE_7 = 0x200D,
	PA_SC_SCREEN_SCISSOR_TL = 0x200E,
	PA_SC_SCREEN_SCISSOR_BR = 0x200F,
	PA_SC_WINDOW_OFFSET = 0x2080,
	PA_SC_WINDOW_SCISSOR_TL = 0x2081,
	PA_SC_WINDOW_SCISSOR_BR = 0x2082,
	VGT_MAX_VTX_INDX = 0x2100,
	VGT_MIN_VTX_INDX = 0x2101,
	VGT_INDX_OFFSET = 0x2102,
	RB_COLOR_MASK = 0x2104,
	PA_CL_VPORT_XSCALE = 0x210F,
	PA_CL_VPORT_XOFFSET = 0x2110,
	PA_CL_VPORT_YSCALE = 0x2111,
	PA_CL_VPORT_YOFFSET = 0x2112,
	PA_CL_VPORT_ZSCALE = 0x2113,
	PA_CL_VPORT_ZOFFSET = 0x2114,
	SQ_PROGRAM_CNTL = 0x2180,
	SQ_CONTEXT_MISC = 0x2181,
	RB_DEPTHCONTROL = 0x2200,
	RB_HIZCONTROL = 0x2203,
	PA_CL_CLIP_CNTL = 0x2204,
	PA_SU_SC_MODE_CNTL = 0x2205,
	PA_CL_VTE_CNTL = 0x2206,
	RB_MODECONTROL = 0x2208,
	PA_SU_POINT_SIZE = 0x2280,
	PA_SU_VTX_CNTL = 0x2302,
	PA_SC_AA_MASK = 0x2312,
	RB_COPY_CONTROL = 0x2318,
	RB_COPY_DEST_BASE = 0x2319,
	RB_COPY_DEST_PITCH = 0x231A,
	RB_COPY_DEST_INFO = 0x231B,
	RB_DEPTH_CLEAR = 0x231D,
	RB_COLOR_CLEAR = 0x231E,
	RB_COLOR_CLEAR_LO = 0x231F,
	SHADER_CONSTANT_FETCH_00_0 = 0x4800,
	SHADER_CONSTANT_FLUSH_FETCH_0 = 0x5000,
};

// Type-3 opcodes (xenia:src/xenia/gpu/xenos.h:1572-1622).
enum {
	PM4_REG_RMW = 0x21,
	PM4_IM_LOAD_IMMEDIATE = 0x2B,
	PM4_DRAW_INDX_2 = 0x36,
	PM4_INVALIDATE_STATE = 0x3B,
	PM4_WAIT_REG_MEM = 0x3C,
	PM4_EVENT_WRITE = 0x46,
	PM4_ME_INIT = 0x48,
	// A type-0 header's bit 15 sends every value to the same register (libxenon:drivers/xenos/xe.c:25-31).
	TYPE0_ONE_REGISTER = 1 << 15,
	PROBE_STAGE_DWORDS = 1280,
};

// Packets a probe builds before copying them into the ring.
typedef struct {
	uint32_t dwords[PROBE_STAGE_DWORDS];
	uint32_t count;
} ProbeStage;

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

static inline void probe_put(ProbeStage *stage, uint32_t value)
{
	assert(stage->count < PROBE_STAGE_DWORDS);
	stage->dwords[stage->count++] = value;
}

static inline void probe_put_registers(ProbeStage *stage, uint32_t index, const uint32_t *values, uint32_t count)
{
	probe_put(stage, (count - 1) << 16 | index);
	for (uint32_t i = 0; i < count; i++)
		probe_put(stage, values[i]);
}

static inline void probe_put_register(ProbeStage *stage, uint32_t index, uint32_t value)
{
	probe_put_registers(stage, index, &value, 1);
}

// Writes value to one register count times in one packet.
static inline void probe_put_register_repeated(ProbeStage *stage, uint32_t index, uint32_t value, uint32_t count)
{
	probe_put(stage, (count - 1) << 16 | TYPE0_ONE_REGISTER | index);
	for (uint32_t i = 0; i < count; i++)
		probe_put(stage, value);
}

static inline void probe_put_type3(ProbeStage *stage, uint32_t opcode, uint32_t payload_dwords)
{
	probe_put(stage, xsdk_gpu_type3(opcode, payload_dwords));
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
