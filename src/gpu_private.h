#ifndef XSDK_GPU_PRIVATE_H
#define XSDK_GPU_PRIVATE_H

#include <assert.h>
#include <stdint.h>

#include "xsdk/gpu.h"

// Register indices (xenia:src/xenia/gpu/register_table.inc), else Xenia's style for ones it doesn't name.
enum {
	UNKNOWN_0081 = 0x0081,
	UNKNOWN_0082 = 0x0082,
	// Named by Xenia (xenia:src/xenia/gpu/graphics_system.cc:211).
	CP_RB_WPTR = 0x01C5,
	WAIT_UNTIL = 0x05C8,
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
	RB_STENCILREFMASK_BF = 0x210C,
	RB_STENCILREFMASK = 0x210D,
	PA_CL_VPORT_XSCALE = 0x210F,
	PA_CL_VPORT_XOFFSET = 0x2110,
	PA_CL_VPORT_YSCALE = 0x2111,
	PA_CL_VPORT_YOFFSET = 0x2112,
	PA_CL_VPORT_ZSCALE = 0x2113,
	PA_CL_VPORT_ZOFFSET = 0x2114,
	SQ_PROGRAM_CNTL = 0x2180,
	SQ_CONTEXT_MISC = 0x2181,
	RB_DEPTHCONTROL = 0x2200,
	RB_BLENDCONTROL0 = 0x2201,
	RB_HIZCONTROL = 0x2203,
	PA_CL_CLIP_CNTL = 0x2204,
	PA_SU_SC_MODE_CNTL = 0x2205,
	PA_CL_VTE_CNTL = 0x2206,
	RB_MODECONTROL = 0x2208,
	PA_SU_POINT_SIZE = 0x2280,
	PA_SC_AA_CONFIG = 0x2301,
	PA_SU_VTX_CNTL = 0x2302,
	SQ_VS_CONST = 0x2307,
	SQ_PS_CONST = 0x2308,
	PA_SC_AA_MASK = 0x2312,
	RB_COPY_CONTROL = 0x2318,
	RB_COPY_DEST_BASE = 0x2319,
	RB_COPY_DEST_PITCH = 0x231A,
	RB_COPY_DEST_INFO = 0x231B,
	RB_HIZ_CLEAR = 0x231C,
	RB_DEPTH_CLEAR = 0x231D,
	RB_COLOR_CLEAR = 0x231E,
	RB_COLOR_CLEAR_LO = 0x231F,
	PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,
	SHADER_CONSTANT_000_X = 0x4000,
	SHADER_CONSTANT_256_X = 0x4400,
	SHADER_CONSTANT_FETCH_00_0 = 0x4800,
	SHADER_CONSTANT_BOOL_000_031 = 0x4900,
	SHADER_CONSTANT_LOOP_00 = 0x4908,
	SHADER_CONSTANT_FLUSH_FETCH_0 = 0x5000,
};

// Type-3 opcodes (xenia:src/xenia/gpu/xenos.h:1572-1622).
enum {
	PM4_REG_RMW = 0x21,
	PM4_DRAW_INDX = 0x22,
	PM4_IM_LOAD = 0x27,
	PM4_IM_LOAD_IMMEDIATE = 0x2B,
	PM4_LOAD_ALU_CONSTANT = 0x2F,
	PM4_DRAW_INDX_2 = 0x36,
	PM4_INVALIDATE_STATE = 0x3B,
	PM4_WAIT_REG_MEM = 0x3C,
	PM4_INDIRECT_BUFFER = 0x3F,
	PM4_EVENT_WRITE = 0x46,
	PM4_ME_INIT = 0x48,
	PM4_EVENT_WRITE_SHD = 0x58,
};

enum {
	// A type-0 header's bit 15 sends every value to the same register (libxenon:drivers/xenos/xe.c:25-31).
	TYPE0_ONE_REGISTER = 1 << 15,
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	SHADER_TYPE_VERTEX = 0,
	SHADER_TYPE_PIXEL = 1,
	// COHER_STATUS_HOST's vc_action_ena and tc_action_ena (xenia:src/xenia/gpu/registers.h:84-85).
	COHER_VERTEX_CACHE = 1 << 24,
	COHER_TEXTURE_CACHE = 1 << 25,
	// RB_HIZCONTROL's bits (xenia-canary:src/xenia/gpu/registers.h:822-831).
	HIZ_WRITE_ENABLE = 1 << 0,
	HIZ_ENABLE = 1 << 1,
	// Turns the test to greater-than, on the console (docs/HARDWARE.md).
	HIZ_GREATER = 1 << 4,
};

// Packets built in memory, then copied into the ring with xsdk_gpu_write.
typedef struct {
	uint32_t *dwords;
	uint32_t count;
	uint32_t capacity;
} GpuPackets;

// GPU registers as titles see them, one dword each (xenia:src/xenia/gpu/graphics_system.cc:92-95).
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static inline void gpu_put(GpuPackets *packets, uint32_t value)
{
	assert(packets->count < packets->capacity);
	packets->dwords[packets->count++] = value;
}

static inline void gpu_put_registers(GpuPackets *packets, uint32_t index, const uint32_t *values, uint32_t count)
{
	gpu_put(packets, (count - 1) << 16 | index);
	for (uint32_t i = 0; i < count; i++)
		gpu_put(packets, values[i]);
}

static inline void gpu_put_register(GpuPackets *packets, uint32_t index, uint32_t value)
{
	gpu_put_registers(packets, index, &value, 1);
}

// Writes value to one register count times in one packet.
static inline void gpu_put_register_repeated(GpuPackets *packets, uint32_t index, uint32_t value, uint32_t count)
{
	gpu_put(packets, (count - 1) << 16 | TYPE0_ONE_REGISTER | index);
	for (uint32_t i = 0; i < count; i++)
		gpu_put(packets, value);
}

static inline void gpu_put_type3(GpuPackets *packets, uint32_t opcode, uint32_t payload_dwords)
{
	gpu_put(packets, xsdk_gpu_type3(opcode, payload_dwords));
}

// Copies the packets into the ring; xsdk_gpu_fence submits them.
void xsdk_gpu_write(const GpuPackets *packets);
// The GPU address of a resolve's rectangle for this size; waits only to replace one a queued resolve may read.
uint32_t xsdk_gpu_rectangle(uint32_t width, uint32_t height);
// Reads the registers the SDK changes, then queues libxenon's set-up.
void xsdk_gpu_set_up(void);
// Queues the set-up again, all but the ME_INIT, after XAM's commands ran.
void xsdk_gpu_set_up_again(void);
// Queues writing back what xsdk_gpu_set_up read.
void xsdk_gpu_put_back(void);
// libxenon's Xe_pSetSurfaceClip with no offset, and its cache invalidation with a wait.
void xsdk_gpu_put_window(GpuPackets *packets, uint32_t width, uint32_t height);
void xsdk_gpu_put_coherency(GpuPackets *packets, uint32_t actions, uint32_t base, uint32_t size_bytes);
// The eDRAM layout for a colour and depth target of this size, with the last target's samples, where it was placed.
void xsdk_gpu_put_surface(GpuPackets *packets, uint32_t width, uint32_t height);
// RB_COPY_CONTROL's sample select that averages the last target's samples.
uint32_t xsdk_gpu_copy_sample_select(void);
// For a resolve: draws need the target set again.
void xsdk_gpu_undo_target(void);
// Whether calls go into a command buffer, not the ring.
bool xsdk_gpu_recording(void);
// After a resolve that cleared depth to RB_DEPTH_CLEAR's depth_clear: the hierarchical buffer bounds that surface's.
void xsdk_gpu_hiz_cleared(uint32_t width, uint32_t height, uint32_t depth_clear);
// Leaves the hierarchical buffer off until depth is next cleared.
void xsdk_gpu_hiz_drop(void);
// Points the shown surface at another buffer of its size; for the vblank callback, so it never sleeps.
void xsdk_display_flip(uint32_t address);

enum {
	DISPLAY_SCALER_REGISTERS = 10,
};

// A surface's size and the scaler that fills the output with it, planned outside the vblank callback.
typedef struct {
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t scaler[DISPLAY_SCALER_REGISTERS];
} DisplaySize;

// Plans a surface of this size filling the output, as the 720p mode's front buffer does; false where the display isn't
// on that mode's scaling (the kernel's mode, an interlaced output).
bool xsdk_display_plan(uint32_t width, uint32_t height, uint32_t pitch, DisplaySize *size);
// The output the display's scaling fills; false where the display isn't on the 720p mode's scaling.
bool xsdk_display_output(uint32_t *width, uint32_t *height);
// As xsdk_display_flip, to a surface of a planned size: the scaler and the surface's size change first when they
// differ from the shown surface's. For the vblank callback.
void xsdk_display_flip_sized(uint32_t address, const DisplaySize *size);

// XAM's surface, while it's to show instead of the title's front buffer.
typedef struct {
	uint32_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
} GpuSurface;

// Hands the frame ending with the front buffer at front_buffer, width by height as xsdk_gpu_resolve writes it, to the
// kernel's swap, before the frame's fence. While the Guide is open, XAM's commands it runs draw the Guide over the
// frame into a surface of XAM's, and the set-up is queued again after them; true, with that surface, while it's to show
// instead of the front buffer (docs/HARDWARE.md, "The Guide shows through VdSwap").
bool xsdk_gpu_swap(volatile const uint32_t *front_buffer, uint32_t width, uint32_t height, GpuSurface *shown);
// For xsdk_present_open: whether the splash's display can be taken over, leaving the logo up, and its size.
bool xsdk_splash_hand_over(uint32_t *width, uint32_t *height);
// Frees a handed-over splash's buffers, once nothing shows them.
void xsdk_splash_free(void);
// A texel's byte offset in Xenos's tiled layout, the pitch a multiple of 32.
uint32_t xsdk_tiled_2d(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_texel_log2);

// What places a texture's levels in memory.
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t levels;
	// Texels across and down a block, as a power of two: 0, or 2 for 4 by 4 blocks.
	uint32_t block_log2;
	uint32_t block_bytes_log2;
	uint32_t base_pitch_blocks;
	uint32_t mip_offset_bytes;
} TextureLayout;

// Where a texture level's block (0, 0) is stored.
typedef struct {
	// From the texture's start: 0 for the base and its tail, else the mip offset plus the levels stored before it.
	uint32_t offset_bytes;
	uint32_t pitch_blocks;
	// Inside the packed tail, in blocks; 0 above it.
	uint32_t x;
	uint32_t y;
} TextureLevelPlace;

// The first level stored in the packed tail, which may be the base.
uint32_t xsdk_texture_packed_level(uint32_t width, uint32_t height);
// Blocks covering texels, rounding up.
uint32_t xsdk_texture_blocks(uint32_t texels, uint32_t block_log2);
// The bytes levels 1 on take at the mip address, for a tiled texture.
uint32_t xsdk_texture_mip_bytes(const TextureLayout *layout);
void xsdk_texture_place(const TextureLayout *layout, uint32_t level, TextureLevelPlace *place);

#endif
