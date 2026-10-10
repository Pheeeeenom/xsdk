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

#ifdef __cplusplus
}
#endif

#endif
