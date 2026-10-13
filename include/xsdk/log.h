#ifndef XSDK_LOG_H
#define XSDK_LOG_H

#include <stddef.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS xsdk_log_open(void);
// Where the log is, as a kernel path ("game:\xsdk_test.log", or a device path); NULL before it opens. Others can open
// it for reading while the title writes it.
const char *xsdk_log_path(void);
void xsdk_log_write(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Writes bytes as they are, without adding a newline; stdout and stderr end up here.
void xsdk_log_write_bytes(const char *bytes, size_t length_bytes);
void xsdk_log_close(void);

#ifdef __cplusplus
}
#endif

#endif
