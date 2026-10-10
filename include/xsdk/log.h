#ifndef XSDK_LOG_H
#define XSDK_LOG_H

#include <stddef.h>

#include "xsdk/kernel.h"

NTSTATUS xsdk_log_open(void);
void xsdk_log_write(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Writes bytes as they are, without adding a newline; stdout and stderr end up here.
void xsdk_log_write_bytes(const char *bytes, size_t length_bytes);
void xsdk_log_close(void);

#endif
