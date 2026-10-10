#ifndef XSDK_FILE_PRIVATE_H
#define XSDK_FILE_PRIVATE_H

#include <sys/stat.h>

#include "xsdk/kernel.h"

enum {
	// NT's MAX_PATH, counting the terminating NUL.
	XSDK_PATH_BYTES = 260,
};

// The errno closest to a kernel status.
int xsdk_file_errno(NTSTATUS status);
// The kernel handle behind an open descriptor, or 0.
HANDLE xsdk_file_handle(int fd);
// Copies the kernel path of a descriptor opened with O_DIRECTORY; returns 0 or an errno.
int xsdk_file_directory_path(int fd, char path[XSDK_PATH_BYTES]);
void xsdk_file_stat(const FILE_NETWORK_OPEN_INFORMATION *information, struct stat *info);

#endif
