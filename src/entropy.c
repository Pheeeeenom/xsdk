#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "xsdk/kernel.h"

// POSIX's GETENTROPY_MAX.
enum { ENTROPY_MAX_BYTES = 256 };

// Xenia fills the buffer with 0xFD instead (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_crypt.cc:354-356).
int getentropy(void *buffer, size_t length_bytes)
{
	if (length_bytes > ENTROPY_MAX_BYTES) {
		errno = EINVAL;
		return -1;
	}
	XeCryptRandom(buffer, (uint32_t)length_bytes);
	return 0;
}
