#ifndef XSDK_SOCKET_PRIVATE_H
#define XSDK_SOCKET_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum {
	// Past src/file.c's descriptors, so sockets stay under FD_SETSIZE for select.
	XSDK_FIRST_SOCKET_FD = 35,
};

// Starts XAM's network once; returns 0, or an errno value if it wouldn't start.
int xsdk_network_start(void);
// Whether fd is an open socket.
bool xsdk_socket_fd(int fd);
// close, read and write on a socket descriptor.
int xsdk_socket_close(int fd);
ssize_t xsdk_socket_read(int fd, void *buffer, size_t length_bytes);
ssize_t xsdk_socket_write(int fd, const void *buffer, size_t length_bytes);

#endif
