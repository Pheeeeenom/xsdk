#ifndef XSDK_SYS_SOCKET_H
#define XSDK_SYS_SOCKET_H

#include <stdint.h>
#include <sys/types.h>

// POSIX sockets on XAM's (src/socket.c), IPv4 only. The numbers are Winsock's, which XAM takes as they are
// (xenia:src/xenia/kernel/xsocket.h:73-91, xsocket.cc:76-95), so most pass through.

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
	sa_family_t sa_family;
	char sa_data[14];
};

// Room for any address, aligned for any of them.
struct sockaddr_storage {
	sa_family_t ss_family;
	char ss_padding[6];
	uint64_t ss_align;
	char ss_rest[112];
};

enum {
	AF_UNSPEC = 0,
	AF_INET = 2,
	PF_UNSPEC = AF_UNSPEC,
	PF_INET = AF_INET,
	SOCK_STREAM = 1,
	SOCK_DGRAM = 2,
	SOL_SOCKET = 0xFFFF,
	SO_REUSEADDR = 0x0004,
	SO_KEEPALIVE = 0x0008,
	SO_BROADCAST = 0x0020,
	SO_SNDBUF = 0x1001,
	SO_RCVBUF = 0x1002,
	// An errno value, as POSIX has it; xsdk works it out, as XAM refuses it (src/socket.c).
	SO_ERROR = 0x1007,
	SO_TYPE = 0x1008,
	MSG_OOB = 0x1,
	MSG_PEEK = 0x2,
	SHUT_RD = 0,
	SHUT_WR = 1,
	SHUT_RDWR = 2,
	SOMAXCONN = 0x7FFFFFFF,
};

// Socket descriptors share the file descriptors' numbers, so close, read and write take them too.
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int connect(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *address, socklen_t *length);
ssize_t send(int fd, const void *buffer, size_t length_bytes, int flags);
ssize_t recv(int fd, void *buffer, size_t length_bytes, int flags);
ssize_t sendto(
	int fd, const void *buffer, size_t length_bytes, int flags, const struct sockaddr *to, socklen_t to_length);
ssize_t recvfrom(int fd, void *buffer, size_t length_bytes, int flags, struct sockaddr *from, socklen_t *from_length);
int shutdown(int fd, int how);
int setsockopt(int fd, int level, int name, const void *value, socklen_t length);
int getsockopt(int fd, int level, int name, void *value, socklen_t *length);
int getsockname(int fd, struct sockaddr *address, socklen_t *length);
int getpeername(int fd, struct sockaddr *address, socklen_t *length);

#ifdef __cplusplus
}
#endif

#endif
