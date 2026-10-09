#ifndef XSDK_LOG_H
#define XSDK_LOG_H

#include "xsdk/kernel.h"

NTSTATUS xsdk_log_open(void);
void xsdk_log_write(const char *format, ...) __attribute__((format(printf, 1, 2)));
void xsdk_log_close(void);

#endif
