#ifndef XSDK_TESTS_H
#define XSDK_TESTS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void check(const char *name, bool passed);

void test_image(void);
void test_threads(void);
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

// Texture formats (milestone 16).
void test_formats(void);

// Vertex input (milestone 17).
void test_vertex(void);

// Recorded command buffers (milestone 18).
void test_replay(void);

#ifdef __cplusplus
}
#endif

#endif
