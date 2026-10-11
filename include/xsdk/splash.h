#ifndef XSDK_SPLASH_H
#define XSDK_SPLASH_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fades the vulkan4xenon logo in over half a second and leaves it up; the GPU must be open. STATUS_NOT_SUPPORTED
// without a 1280x720 display. xsdk_present_open then takes the display over, and the logo stays until its first frame.
NTSTATUS xsdk_splash_begin(void);
// Takes the logo down and closes the display, unless xsdk_present_open has taken it over.
void xsdk_splash_end(void);
// Fades the logo in, holds it until duration_ms have passed and lets the GPU go; call with the GPU closed.
NTSTATUS xsdk_splash_show(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif
