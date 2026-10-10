#ifndef XSDK_PATH_PRIVATE_H
#define XSDK_PATH_PRIVATE_H

#include "file_private.h"
#include "xsdk/kernel.h"

// Turns a POSIX path, relative to dir_fd or the current directory, into a kernel path; returns 0 or an errno.
int xsdk_path_resolve(int dir_fd, const char *path, char resolved[XSDK_PATH_BYTES]);
// Points name and attributes at a resolved path, which must outlive them.
void xsdk_path_attributes(const char *resolved, ANSI_STRING *name, OBJECT_ATTRIBUTES *attributes);

#endif
