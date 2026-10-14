#ifndef XSDK_POLL_H
#define XSDK_POLL_H

// Waits on sockets through XAM's select (src/socket.c).

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int nfds_t;

struct pollfd {
	int fd;
	short events;
	short revents;
};

enum {
	POLLIN = 0x0001,
	POLLPRI = 0x0002,
	POLLOUT = 0x0004,
	POLLERR = 0x0008,
	POLLHUP = 0x0010,
	POLLNVAL = 0x0020,
};

// Socket descriptors only; a negative fd is skipped, as POSIX has it, and any other gets POLLNVAL.
int poll(struct pollfd *fds, nfds_t count, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
