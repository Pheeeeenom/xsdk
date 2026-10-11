#ifndef XSDK_SPLASH_H
#define XSDK_SPLASH_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fades the vulkan4xenon logo in, holds it for duration_ms and lets the GPU go; call with the GPU closed.
NTSTATUS xsdk_splash_show(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif
