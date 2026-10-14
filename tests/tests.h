#ifndef XSDK_TESTS_H
#define XSDK_TESTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void check(const char *name, bool passed);

void test_image(void);
// The memory the kernel leaves a title (milestone 21).
void test_memory(void);
void test_threads(void);
// Threads placed on the six hardware threads (milestone 26).
void test_placement(void);
void test_pthread(void);
void test_clocks(void);
void test_kernel_entry(void);

// The registers the console's code keeps across a call, r14-r31 whole, f14-f31 and v14-v31, as kernel_entry_check.S
// sets and reads them; bit 14 + i of each mask in EntryChanged stands for register 14 + i.
enum {
	ENTRY_REGISTERS = 18,
	ENTRY_FIRST_KEPT = 14,
};

typedef struct {
	uint64_t gpr[ENTRY_REGISTERS];
	uint64_t fpr[ENTRY_REGISTERS];
	uint32_t vr[ENTRY_REGISTERS][4];
} EntryRegs;

typedef struct {
	uint32_t gpr_low;
	uint32_t gpr_high;
	uint32_t fpr;
	uint32_t vr;
} EntryChanged;

// Sets them to sentinels, calls fn(arg) as the kernel calls a title's code, and stores what they hold after in *out.
void test_entry_regs_call(void (*fn)(void *), void *arg, EntryRegs *out);
// Stores them as they are into *out, 16-byte aligned, and changes none of them.
void test_entry_snapshot(EntryRegs *out);
// Which of them differ from the sentinels test_entry_regs_call set.
EntryChanged test_entry_changed(const EntryRegs *regs);
void test_files(void);
// The SDK's memcpy and memmove (milestone 20).
void test_copy(void);
void test_input(void);
void test_audio(void);

// C++23 runtime (milestone 3).
void test_cpp_objects(void);
void test_cpp_threads(void);
void test_cpp_atomics(void);
void test_cpp_exceptions(void);
// Registers a caller keeps, through a C++ exception and through longjmp (milestone 22).
void test_cpp_unwind(void);
// Floats and doubles at every byte offset, which the console faults on in float registers (milestone 23).
void test_fp_align(void);
void test_cpp_chrono(void);
void test_cpp_filesystem(void);
void test_cpp_format(void);

// The command processor (milestone 4).
void test_gpu(void);

// A cleared screen (milestone 5).
void test_present(void);

// A triangle (milestone 6).
void test_triangle(void);

// A textured quad (milestone 7).
void test_texture(void);

// Render to texture (milestone 8).
void test_render(void);

// Frame pacing (milestone 9), each mode briefly; tests/pacing.c runs one for 10 minutes.
void test_pacing(void);

// 1280x720 on the display's scaler, held on screen for a capture.
void test_scale(void);

// Shader constants (milestone 11).
void test_constants(void);

// Indexed draws and topologies (milestone 12).
void test_indexed(void);

// Depth and stencil (milestone 13).
void test_depth(void);

// Raster and blend state (milestone 14).
void test_raster(void);

// Sampling and mipmaps (milestone 15).
void test_sampling(void);

// Anisotropic filtering (milestone 33).
void test_anisotropy(void);

// Texture formats (milestone 16).
void test_formats(void);

// Multisampled targets (milestone 32).
void test_multisample(void);

// Vertex input (milestone 17).
void test_vertex(void);

// Recorded command buffers (milestone 18).
void test_replay(void);

// Shaders' microcode in shared pages (milestone 25).
void test_microcode(void);

// Shaders past 16 registers (milestone 24). Last, as without xsdk's register split it hangs the GPU.
void test_registers(void);

#ifdef __cplusplus
}
#endif

#endif
