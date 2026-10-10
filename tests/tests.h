#ifndef XSDK_TESTS_H
#define XSDK_TESTS_H

#include <stdbool.h>

void check(const char *name, bool passed);

void test_image(void);
void test_threads(void);
void test_files(void);
void test_input(void);
void test_audio(void);

#endif
