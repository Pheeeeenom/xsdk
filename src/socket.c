#include "socket_private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "file_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// POSIX sockets on XAM's (xenia:src/xenia/kernel/xam/xam_net.cc). Each is marked insecure as it's made: on the console
// only such sockets reach the LAN or the internet (docs/HARDWARE.md, "Sockets reach the LAN and the internet when
// marked insecure").

enum {
	SOCKET_SLOTS = FD_SETSIZE - XSDK_FIRST_SOCKET_FD,
	CONSOLE_FDS = 3,
	INVALID_SOCKET = -1,
	// Xenia takes it as disabling a socket's encryption (xenia:src/xenia/kernel/xsocket.cc:78-82).
	OPTION_MARK_INSECURE = 0x5801,
	WINSOCK_VERSION = 0x0202,
	// XNetStartupParams: its size first, its settings 0 (xenia:src/xenia/kernel/xam/xam_net.cc:154-168). The flags
	// changed nothing on the console (docs/HARDWARE.md).
	STARTUP_PARAMS_BYTES = 13,
	// WSADATA is 400 bytes as Xenia lays it out (xam_net.cc:90-98).
	WSADATA_BYTES = 400,
	MILLISECONDS_PER_SECOND = 1000,
	MICROSECONDS_PER_MILLISECOND = 1000,
};

// Winsock's FIONBIO, which XAM takes (docs/HARDWARE.md).
static const uint32_t FIONBIO = 0x8004667E;

// Winsock's errors, which XAM gives: Xenia's own are them, WSAEFAULT 0x271E and WSAENOTSOCK 0x2736 (xam_net.cc:651,
// 790), and it passes the host's on (xam_net.cc:668-676).
enum {
	WSAEINTR = 10004,
	WSAEBADF = 10009,
	WSAEACCES = 10013,
	WSAEFAULT = 10014,
	WSAEINVAL = 10022,
	WSAEMFILE = 10024,
	WSAEWOULDBLOCK = 10035,
	WSAEINPROGRESS = 10036,
	WSAEALREADY = 10037,
	WSAENOTSOCK = 10038,
	WSAEDESTADDRREQ = 10039,
	WSAEMSGSIZE = 10040,
	WSAEPROTOTYPE = 10041,
	WSAENOPROTOOPT = 10042,
	WSAEPROTONOSUPPORT = 10043,
	WSAESOCKTNOSUPPORT = 10044,
	WSAEOPNOTSUPP = 10045,
	WSAEPFNOSUPPORT = 10046,
	WSAEAFNOSUPPORT = 10047,
	WSAEADDRINUSE = 10048,
	WSAEADDRNOTAVAIL = 10049,
	WSAENETDOWN = 10050,
	WSAENETUNREACH = 10051,
	WSAENETRESET = 10052,
	WSAECONNABORTED = 10053,
	WSAECONNRESET = 10054,
	WSAENOBUFS = 10055,
	WSAEISCONN = 10056,
	WSAENOTCONN = 10057,
	WSAESHUTDOWN = 10058,
	WSAETIMEDOUT = 10060,
	WSAECONNREFUSED = 10061,
	WSAEHOSTUNREACH = 10065,
	WSANOTINITIALISED = 10093,
};

enum {
	SLOT_FREE,
	SLOT_CLAIMED,
	SLOT_OPEN,
};

typedef struct {
	uint32_t state;
	int32_t handle;
	bool non_blocking;
} SocketSlot;

static SocketSlot sockets[SOCKET_SLOTS];
static pthread_once_t network_once = PTHREAD_ONCE_INIT;
static int network_error;

static void start_network(void)
{
	const uint8_t params[STARTUP_PARAMS_BYTES] = { STARTUP_PARAMS_BYTES };
	const int32_t started = NetDll_XNetStartup(XNCALLER_TITLE, params);
	if (started != 0) {
		xsdk_log_write("socket: XNetStartup failed: %d", (int)started);
		network_error = ENETDOWN;
		return;
	}
	static uint8_t data[WSADATA_BYTES];
	const int32_t error = NetDll_WSAStartup(XNCALLER_TITLE, WINSOCK_VERSION, data);
	if (error != 0) {
		xsdk_log_write("socket: WSAStartup failed: %d", (int)error);
		network_error = ENETDOWN;
	}
}

int xsdk_network_start(void)
{
	(void)pthread_once(&network_once, start_network);
	return network_error;
}

static int errno_from_winsock(int32_t error)
{
	switch (error) {
	case WSAEINTR:
		return EINTR;
	case WSAEBADF:
		return EBADF;
	case WSAEACCES:
		return EACCES;
	case WSAEFAULT:
		return EFAULT;
	case WSAEINVAL:
		return EINVAL;
	case WSAEMFILE:
		return EMFILE;
	case WSAEWOULDBLOCK:
		return EAGAIN;
	case WSAEINPROGRESS:
		return EINPROGRESS;
	case WSAEALREADY:
		return EALREADY;
	case WSAENOTSOCK:
		return ENOTSOCK;
	case WSAEDESTADDRREQ:
		return EDESTADDRREQ;
	case WSAEMSGSIZE:
		return EMSGSIZE;
	case WSAEPROTOTYPE:
	case WSAESOCKTNOSUPPORT:
		return EPROTOTYPE;
	case WSAENOPROTOOPT:
		return ENOPROTOOPT;
	case WSAEPROTONOSUPPORT:
		return EPROTONOSUPPORT;
	case WSAEOPNOTSUPP:
		return EOPNOTSUPP;
	case WSAEPFNOSUPPORT:
		return EPFNOSUPPORT;
	case WSAEAFNOSUPPORT:
		return EAFNOSUPPORT;
	case WSAEADDRINUSE:
		return EADDRINUSE;
	case WSAEADDRNOTAVAIL:
		return EADDRNOTAVAIL;
	case WSAENETDOWN:
	case WSANOTINITIALISED:
		return ENETDOWN;
	case WSAENETUNREACH:
		return ENETUNREACH;
	case WSAENETRESET:
		return ENETRESET;
	case WSAECONNABORTED:
		return ECONNABORTED;
	case WSAECONNRESET:
		return ECONNRESET;
	case WSAENOBUFS:
		return ENOBUFS;
	case WSAEISCONN:
		return EISCONN;
	case WSAENOTCONN:
		return ENOTCONN;
	// Sending after a shutdown is EPIPE in POSIX.
	case WSAESHUTDOWN:
		return EPIPE;
	case WSAETIMEDOUT:
		return ETIMEDOUT;
	case WSAECONNREFUSED:
		return ECONNREFUSED;
	case WSAEHOSTUNREACH:
		return EHOSTUNREACH;
	default:
		return EIO;
	}
}

// Sets errno from XAM's last error; returns -1.
static int fail_from_xam(void)
{
	errno = errno_from_winsock(NetDll_WSAGetLastError());
	return -1;
}

static SocketSlot *claim_slot(int *fd)
{
	for (int i = 0; i < SOCKET_SLOTS; i++) {
		uint32_t expected = SLOT_FREE;
		if (__atomic_compare_exchange_n(
				&sockets[i].state, &expected, SLOT_CLAIMED, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			*fd = XSDK_FIRST_SOCKET_FD + i;
			return &sockets[i];
		}
	}
	errno = EMFILE;
	return NULL;
}

static void release_slot(SocketSlot *slot)
{
	__atomic_store_n(&slot->state, SLOT_FREE, __ATOMIC_RELEASE);
}

static void open_slot(SocketSlot *slot, int32_t handle, bool non_blocking)
{
	slot->handle = handle;
	slot->non_blocking = non_blocking;
	__atomic_store_n(&slot->state, SLOT_OPEN, __ATOMIC_RELEASE);
}

static SocketSlot *open_slot_of(int fd)
{
	if (fd < XSDK_FIRST_SOCKET_FD || fd >= XSDK_FIRST_SOCKET_FD + SOCKET_SLOTS)
		return NULL;
	SocketSlot *slot = &sockets[fd - XSDK_FIRST_SOCKET_FD];
	return __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == SLOT_OPEN ? slot : NULL;
}

// The socket behind fd, or NULL with errno ENOTSOCK for another open descriptor and EBADF otherwise.
static SocketSlot *find_socket(int fd)
{
	SocketSlot *slot = open_slot_of(fd);
	if (!slot)
		errno = (fd >= 0 && fd < CONSOLE_FDS) || xsdk_file_handle(fd) ? ENOTSOCK : EBADF;
	return slot;
}

bool xsdk_socket_fd(int fd)
{
	return open_slot_of(fd) != NULL;
}

static int32_t clamp_length(size_t length_bytes)
{
	return length_bytes > INT32_MAX ? INT32_MAX : (int32_t)length_bytes;
}

int socket(int domain, int type, int protocol)
{
	const int error = xsdk_network_start();
	if (error) {
		errno = error;
		return -1;
	}
	int fd;
	SocketSlot *slot = claim_slot(&fd);
	if (!slot)
		return -1;
	const int32_t handle = NetDll_socket(XNCALLER_TITLE, domain, type, protocol);
	if (handle == INVALID_SOCKET) {
		release_slot(slot);
		return fail_from_xam();
	}
	const uint32_t on = 1;
	if (NetDll_setsockopt(XNCALLER_TITLE, handle, SOL_SOCKET, OPTION_MARK_INSECURE, &on, sizeof(on)) != 0) {
		const int32_t mark_error = NetDll_WSAGetLastError();
		xsdk_log_write("socket: marking it insecure failed: %d", (int)mark_error);
		(void)NetDll_closesocket(XNCALLER_TITLE, handle);
		release_slot(slot);
		errno = errno_from_winsock(mark_error);
		return -1;
	}
	open_slot(slot, handle, false);
	return fd;
}

int xsdk_socket_close(int fd)
{
	SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	const int32_t handle = slot->handle;
	release_slot(slot);
	return NetDll_closesocket(XNCALLER_TITLE, handle) == 0 ? 0 : fail_from_xam();
}

int bind(int fd, const struct sockaddr *address, socklen_t length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	return NetDll_bind(XNCALLER_TITLE, slot->handle, address, (int32_t)length) == 0 ? 0 : fail_from_xam();
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	if (NetDll_connect(XNCALLER_TITLE, slot->handle, address, (int32_t)length) == 0)
		return 0;
	const int32_t error = NetDll_WSAGetLastError();
	// Winsock says a non-blocking connect would block, where POSIX says it's in progress.
	errno = error == WSAEWOULDBLOCK ? EINPROGRESS : errno_from_winsock(error);
	return -1;
}

int listen(int fd, int backlog)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	return NetDll_listen(XNCALLER_TITLE, slot->handle, backlog) == 0 ? 0 : fail_from_xam();
}

int accept(int fd, struct sockaddr *address, socklen_t *length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	int accepted_fd;
	SocketSlot *accepted = claim_slot(&accepted_fd);
	if (!accepted)
		return -1;
	// XAM wants somewhere to put the address (xam_net.cc:789-793), where POSIX lets it be left out.
	struct sockaddr_storage from;
	int32_t from_length = sizeof(from);
	const int32_t handle = NetDll_accept(XNCALLER_TITLE, slot->handle, &from, &from_length);
	if (handle == INVALID_SOCKET) {
		release_slot(accepted);
		return fail_from_xam();
	}
	if (address && length) {
		memcpy(address, &from, *length < (socklen_t)from_length ? *length : (socklen_t)from_length);
		*length = (socklen_t)from_length;
	}
	// A Winsock socket takes its listener's blocking mode.
	open_slot(accepted, handle, slot->non_blocking);
	return accepted_fd;
}

ssize_t send(int fd, const void *buffer, size_t length_bytes, int flags)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	const int32_t sent = NetDll_send(XNCALLER_TITLE, slot->handle, buffer, clamp_length(length_bytes), flags);
	return sent >= 0 ? sent : fail_from_xam();
}

ssize_t recv(int fd, void *buffer, size_t length_bytes, int flags)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	const int32_t received = NetDll_recv(XNCALLER_TITLE, slot->handle, buffer, clamp_length(length_bytes), flags);
	return received >= 0 ? received : fail_from_xam();
}

ssize_t sendto(
	int fd, const void *buffer, size_t length_bytes, int flags, const struct sockaddr *to, socklen_t to_length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	const int32_t sent =
		NetDll_sendto(XNCALLER_TITLE, slot->handle, buffer, clamp_length(length_bytes), flags, to, (int32_t)to_length);
	return sent >= 0 ? sent : fail_from_xam();
}

ssize_t recvfrom(int fd, void *buffer, size_t length_bytes, int flags, struct sockaddr *from, socklen_t *from_length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	int32_t length = from_length ? (int32_t)*from_length : 0;
	const int32_t received = NetDll_recvfrom(
		XNCALLER_TITLE, slot->handle, buffer, clamp_length(length_bytes), flags, from, from_length ? &length : NULL);
	if (received < 0)
		return fail_from_xam();
	if (from_length)
		*from_length = (socklen_t)length;
	return received;
}

ssize_t xsdk_socket_read(int fd, void *buffer, size_t length_bytes)
{
	return recv(fd, buffer, length_bytes, 0);
}

ssize_t xsdk_socket_write(int fd, const void *buffer, size_t length_bytes)
{
	return send(fd, buffer, length_bytes, 0);
}

int shutdown(int fd, int how)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	return NetDll_shutdown(XNCALLER_TITLE, slot->handle, how) == 0 ? 0 : fail_from_xam();
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	return NetDll_setsockopt(XNCALLER_TITLE, slot->handle, level, name, value, (int32_t)length) == 0 ? 0
																									 : fail_from_xam();
}

// XAM refuses SO_ERROR, and none of the options it gives says why a connection failed; its select says only that it
// did, putting the socket in its error set (docs/HARDWARE.md). So SO_ERROR is 0 until then, and EIO, an error of no
// named cause, after.
static int connection_error(const SocketSlot *slot, void *value, socklen_t *length)
{
	if (*length < sizeof(int)) {
		errno = EINVAL;
		return -1;
	}
	WSA_FD_SET failed = { .count = 1, .sockets = { slot->handle } };
	const WSA_TIMEVAL now = { 0 };
	if (NetDll_select(XNCALLER_TITLE, 0, NULL, NULL, &failed, &now) < 0)
		return fail_from_xam();
	*(int *)value = failed.count ? EIO : 0;
	*length = sizeof(int);
	return 0;
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	if (level == SOL_SOCKET && name == SO_ERROR)
		return connection_error(slot, value, length);
	int32_t got_length = (int32_t)*length;
	if (NetDll_getsockopt(XNCALLER_TITLE, slot->handle, level, name, value, &got_length) != 0)
		return fail_from_xam();
	*length = (socklen_t)got_length;
	return 0;
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	int32_t got_length = (int32_t)*length;
	if (NetDll_getsockname(XNCALLER_TITLE, slot->handle, address, &got_length) != 0)
		return fail_from_xam();
	*length = (socklen_t)got_length;
	return 0;
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length)
{
	const SocketSlot *slot = find_socket(fd);
	if (!slot)
		return -1;
	int32_t got_length = (int32_t)*length;
	if (NetDll_getpeername(XNCALLER_TITLE, slot->handle, address, &got_length) != 0)
		return fail_from_xam();
	*length = (socklen_t)got_length;
	return 0;
}

// Sockets only so far; for files it fails with ENOSYS.
int fcntl(int fd, int command, ...)
{
	SocketSlot *slot = open_slot_of(fd);
	if (!slot) {
		errno = (fd >= 0 && fd < CONSOLE_FDS) || xsdk_file_handle(fd) ? ENOSYS : EBADF;
		return -1;
	}
	switch (command) {
	case F_GETFL:
		return O_RDWR | (slot->non_blocking ? O_NONBLOCK : 0);
	case F_SETFL: {
		va_list arguments;
		va_start(arguments, command);
		const int flags = va_arg(arguments, int);
		va_end(arguments);
		uint32_t non_blocking = (flags & O_NONBLOCK) != 0;
		if (NetDll_ioctlsocket(XNCALLER_TITLE, slot->handle, FIONBIO, &non_blocking) != 0)
			return fail_from_xam();
		slot->non_blocking = non_blocking != 0;
		return 0;
	}
	// Titles don't run other programs, so close-on-exec changes nothing.
	case F_GETFD:
	case F_SETFD:
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

static bool add_to_set(WSA_FD_SET *set, int32_t handle)
{
	if (set->count == WSA_FD_SET_SOCKETS)
		return false;
	set->sockets[set->count++] = handle;
	return true;
}

static bool in_set(const WSA_FD_SET *set, int32_t handle)
{
	for (uint32_t i = 0; i < set->count; i++) {
		if (set->sockets[i] == handle)
			return true;
	}
	return false;
}

// Waits without sockets: as long as asked, or never, which can't end.
static int wait_without_sockets(int32_t timeout_ms)
{
	if (timeout_ms < 0) {
		errno = EINVAL;
		return -1;
	}
	(void)xsdk_thread_sleep((uint32_t)timeout_ms);
	return 0;
}

static WSA_TIMEVAL wait_time(int32_t timeout_ms)
{
	return (WSA_TIMEVAL){ .seconds = timeout_ms / MILLISECONDS_PER_SECOND,
		.microseconds = timeout_ms % MILLISECONDS_PER_SECOND * MICROSECONDS_PER_MILLISECOND };
}

int poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
	WSA_FD_SET readable = { 0 };
	WSA_FD_SET writable = { 0 };
	WSA_FD_SET failed = { 0 };
	int invalid = 0;
	bool any = false;
	for (nfds_t i = 0; i < count; i++) {
		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue;
		const SocketSlot *slot = open_slot_of(fds[i].fd);
		if (!slot) {
			fds[i].revents = POLLNVAL;
			invalid++;
			continue;
		}
		const bool added = (!(fds[i].events & (POLLIN | POLLPRI)) || add_to_set(&readable, slot->handle)) &&
			(!(fds[i].events & POLLOUT) || add_to_set(&writable, slot->handle)) && add_to_set(&failed, slot->handle);
		if (!added) {
			errno = EINVAL;
			return -1;
		}
		any = true;
	}
	if (!any)
		return invalid ? invalid : wait_without_sockets(timeout_ms);
	// A descriptor already marked invalid counts as ready, so the wait mustn't block.
	const WSA_TIMEVAL timeout = wait_time(invalid ? 0 : timeout_ms);
	if (NetDll_select(XNCALLER_TITLE, 0, &readable, &writable, &failed, timeout_ms < 0 && !invalid ? NULL : &timeout) <
		0)
		return fail_from_xam();
	int ready = invalid;
	for (nfds_t i = 0; i < count; i++) {
		const SocketSlot *slot = fds[i].fd >= 0 ? open_slot_of(fds[i].fd) : NULL;
		if (!slot)
			continue;
		if (in_set(&readable, slot->handle))
			fds[i].revents |= (short)(fds[i].events & (POLLIN | POLLPRI));
		if (in_set(&writable, slot->handle))
			fds[i].revents |= POLLOUT;
		if (in_set(&failed, slot->handle))
			fds[i].revents |= POLLERR;
		ready += fds[i].revents != 0;
	}
	return ready;
}

int select(int count, fd_set *read, fd_set *write, fd_set *error, struct timeval *timeout)
{
	fd_set *const sets[] = { read, write, error };
	WSA_FD_SET wanted[3] = { 0 };
	int32_t handles[FD_SETSIZE];
	bool any = false;
	for (int fd = 0; fd < count && fd < FD_SETSIZE; fd++) {
		const SocketSlot *slot = NULL;
		for (int s = 0; s < 3; s++) {
			if (!sets[s] || !FD_ISSET(fd, sets[s]))
				continue;
			if (!slot && !(slot = open_slot_of(fd))) {
				errno = EBADF;
				return -1;
			}
			handles[fd] = slot->handle;
			(void)add_to_set(&wanted[s], slot->handle);
			any = true;
		}
	}
	const int32_t timeout_ms = timeout
		? (int32_t)(timeout->tv_sec * MILLISECONDS_PER_SECOND + timeout->tv_usec / MICROSECONDS_PER_MILLISECOND)
		: -1;
	if (!any)
		return wait_without_sockets(timeout_ms);
	WSA_TIMEVAL wait = wait_time(timeout_ms);
	if (NetDll_select(XNCALLER_TITLE, 0, &wanted[0], &wanted[1], &wanted[2], timeout ? &wait : NULL) < 0)
		return fail_from_xam();
	int ready = 0;
	for (int fd = 0; fd < count && fd < FD_SETSIZE; fd++) {
		for (int s = 0; s < 3; s++) {
			if (!sets[s] || !FD_ISSET(fd, sets[s]))
				continue;
			if (in_set(&wanted[s], handles[fd]))
				ready++;
			else
				FD_CLR(fd, sets[s]);
		}
	}
	return ready;
}
