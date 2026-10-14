#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tests.h"
#include "xsdk/log.h"

// Sockets against tools/net_echo.py on the PC, whose address and port net_host.txt beside the .xex holds, as
// "192.168.0.125 7370" (docs/scope/networking.md, step 2).

enum {
	TIMEOUT_MS = 3000,
	ACCEPT_TIMEOUT_MS = 5000,
	LINE_BYTES = 128,
	LISTEN_PORT = 7373,
	// Nothing listens there on the console.
	CLOSED_PORT = 7379,
	// Far more than XAM holds at once, opened and closed in turn.
	SOCKETS_IN_TURN = 1000,
};

typedef struct {
	struct sockaddr_in address;
	char text[INET_ADDRSTRLEN];
} Host;

static struct sockaddr_in socket_address(in_addr_t address, in_port_t port)
{
	return (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = { htonl(address) } };
}

static bool read_host(Host *pc)
{
	FILE *file = fopen("net_host.txt", "r");
	unsigned port = 0;
	in_addr_t address = 0;
	const bool read =
		file && fscanf(file, "%15s %u", pc->text, &port) == 2 && inet_pton(AF_INET, pc->text, &address) == 1;
	if (file)
		fclose(file);
	if (read)
		pc->address = socket_address(ntohl(address), (in_port_t)port);
	else
		xsdk_log_write("net: net_host.txt beside the .xex must hold \"<PC address> <port>\"");
	return read;
}

static bool wait_for(int fd, short events, int timeout_ms)
{
	struct pollfd wanted = { .fd = fd, .events = events };
	const int ready = poll(&wanted, 1, timeout_ms);
	if (ready < 0)
		xsdk_log_write("net: poll failed: %d", errno);
	return ready == 1 && (wanted.revents & events);
}

// Sends a line and returns whether the same comes back.
static bool echoes(int fd, const char *line)
{
	const size_t length = strlen(line);
	if (send(fd, line, length, 0) != (ssize_t)length) {
		xsdk_log_write("net: send failed: %d", errno);
		return false;
	}
	char reply[LINE_BYTES];
	if (!wait_for(fd, POLLIN, TIMEOUT_MS)) {
		xsdk_log_write("net: nothing came back in %d ms", TIMEOUT_MS);
		return false;
	}
	const ssize_t received = recv(fd, reply, sizeof(reply) - 1, 0);
	if (received < 0)
		xsdk_log_write("net: recv failed: %d", errno);
	return received == (ssize_t)length && memcmp(reply, line, length) == 0;
}

// A blocking connection to the PC, or -1.
static int connect_to(const Host *pc)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		xsdk_log_write("net: socket failed: %d", errno);
		return -1;
	}
	if (connect(fd, (const struct sockaddr *)&pc->address, sizeof(pc->address)) != 0) {
		xsdk_log_write("net: connect to %s failed: %d", pc->text, errno);
		close(fd);
		return -1;
	}
	return fd;
}

// The console's own address, as the PC sees it: getsockname gives 0.0.0.0 for a connected socket on the console.
static bool ask_address(const Host *pc, in_addr_t *address)
{
	const int fd = connect_to(pc);
	char reply[LINE_BYTES];
	const ssize_t received = fd >= 0 && send(fd, "addr\n", 5, 0) == 5 && wait_for(fd, POLLIN, TIMEOUT_MS)
		? recv(fd, reply, sizeof(reply) - 1, 0)
		: -1;
	if (fd >= 0)
		close(fd);
	if (received <= 0)
		return false;
	reply[received] = 0;
	reply[strcspn(reply, "\r\n")] = 0;
	return inet_pton(AF_INET, reply, address) == 1;
}

static void test_tcp(const Host *pc, struct sockaddr_in *console)
{
	const int fd = connect_to(pc);
	struct sockaddr_in local = { 0 };
	socklen_t length = sizeof(local);
	char text[INET_ADDRSTRLEN] = "";
	if (fd >= 0 && getsockname(fd, (struct sockaddr *)&local, &length) == 0)
		inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
	xsdk_log_write(
		"net: connected to %s; getsockname gives %s, port %u", pc->text, text, (unsigned)ntohs(local.sin_port));
	check("net: TCP to the PC and back, through send and recv", fd >= 0 && echoes(fd, "tcp through send\n"));
	if (fd >= 0)
		close(fd);

	in_addr_t address = 0;
	if (ask_address(pc, &address))
		*console = socket_address(ntohl(address), 0);
	inet_ntop(AF_INET, &console->sin_addr, text, sizeof(text));
	xsdk_log_write("net: the PC sees the console at %s", text);

	// The same through the descriptor calls.
	const int second = connect_to(pc);
	struct stat info;
	const char line[] = "tcp through write\n";
	char reply[LINE_BYTES];
	const bool written = second >= 0 && fstat(second, &info) == 0 && S_ISSOCK(info.st_mode) &&
		write(second, line, strlen(line)) == (ssize_t)strlen(line) && wait_for(second, POLLIN, TIMEOUT_MS) &&
		read(second, reply, sizeof(reply)) == (ssize_t)strlen(line) && memcmp(reply, line, strlen(line)) == 0;
	const bool closed = second >= 0 && close(second) == 0 && read(second, reply, 1) < 0 && errno == EBADF;
	check("net: a socket is a descriptor: fstat, write, read and close", written && closed);
}

static void test_udp(const Host *pc)
{
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	const char line[] = "udp through sendto\n";
	char reply[LINE_BYTES];
	struct sockaddr_in from;
	socklen_t from_length = sizeof(from);
	const bool echoed = fd >= 0 &&
		sendto(fd, line, strlen(line), 0, (const struct sockaddr *)&pc->address, sizeof(pc->address)) ==
			(ssize_t)strlen(line) &&
		wait_for(fd, POLLIN, TIMEOUT_MS) &&
		recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, &from_length) == (ssize_t)strlen(line) &&
		memcmp(reply, line, strlen(line)) == 0 && from.sin_addr.s_addr == pc->address.sin_addr.s_addr;
	if (fd >= 0)
		close(fd);
	check("net: UDP to the PC and back, through sendto and recvfrom", echoed);
}

// Listens, asks the PC to connect back, and takes its line.
static void test_accept(const Host *pc)
{
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	const struct sockaddr_in any = socket_address(INADDR_ANY, LISTEN_PORT);
	bool accepted = false;
	if (listener >= 0 && bind(listener, (const struct sockaddr *)&any, sizeof(any)) == 0 && listen(listener, 1) == 0) {
		const int asker = connect_to(pc);
		char request[LINE_BYTES];
		snprintf(request, sizeof(request), "back %d\n", LISTEN_PORT);
		char reply[LINE_BYTES];
		const bool asked = asker >= 0 && send(asker, request, strlen(request), 0) == (ssize_t)strlen(request) &&
			wait_for(asker, POLLIN, TIMEOUT_MS) && recv(asker, reply, sizeof(reply), 0) > 0;
		if (asker >= 0)
			close(asker);
		if (asked && wait_for(listener, POLLIN, ACCEPT_TIMEOUT_MS)) {
			const int connection = accept(listener, NULL, NULL);
			const ssize_t received = connection >= 0 && wait_for(connection, POLLIN, TIMEOUT_MS)
				? recv(connection, reply, sizeof(reply) - 1, 0)
				: -1;
			accepted = received > 0 && strncmp(reply, "hello from the PC", 17) == 0;
			if (connection >= 0)
				close(connection);
		}
	}
	if (listener >= 0)
		close(listener);
	check("net: a connection from the PC, accepted", accepted);
}

static void test_names(void)
{
	const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *found = NULL;
	const int error = getaddrinfo("example.com", "80", &hints, &found);
	char text[INET_ADDRSTRLEN] = "";
	const struct sockaddr_in *address = found ? (const struct sockaddr_in *)found->ai_addr : NULL;
	if (address)
		inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
	xsdk_log_write("net: example.com: %s %s", error ? gai_strerror(error) : "found at", text);
	check("net: getaddrinfo looks a host name up",
		error == 0 && address && address->sin_port == htons(80) && address->sin_addr.s_addr != 0 &&
			found->ai_protocol == IPPROTO_TCP);
	freeaddrinfo(found);

	in_addr_t parsed = 0;
	char round[INET_ADDRSTRLEN] = "";
	const bool converted = inet_pton(AF_INET, "192.168.0.125", &parsed) == 1 && parsed == 0xC0A8007D &&
		inet_ntop(AF_INET, &parsed, round, sizeof(round)) && strcmp(round, "192.168.0.125") == 0 &&
		inet_pton(AF_INET, "256.1.1.1", &parsed) == 0 && inet_pton(AF_INET, "1.2.3", &parsed) == 0;
	check("net: inet_pton and inet_ntop take dotted addresses, and refuse others", converted);
}

// Starts a non-blocking connection and waits for poll to say how it went; gives poll's events and SO_ERROR.
static bool connect_without_blocking(int fd, const struct sockaddr_in *to, short *events, int *error)
{
	const bool set = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK);
	const int connected = set ? connect(fd, (const struct sockaddr *)to, sizeof(*to)) : -1;
	const int connect_error = connected == 0 ? 0 : errno;
	struct pollfd wanted = { .fd = fd, .events = POLLOUT };
	const int ready = set && (connected == 0 || connect_error == EINPROGRESS) ? poll(&wanted, 1, TIMEOUT_MS) : -1;
	socklen_t length = sizeof(*error);
	const bool got = ready == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, error, &length) == 0;
	*events = wanted.revents;
	if (!got)
		xsdk_log_write("net: non-blocking connect %d, errno %d; poll %d, revents %04X; SO_ERROR not read: %d",
			connected, connect_error, ready, (unsigned)wanted.revents, errno);
	return got;
}

// A closed port on the console itself: XAM refuses it at once, where the PC's firewall may let a connection time out.
static void test_refused(const struct sockaddr_in *console)
{
	const struct sockaddr_in closed = socket_address(ntohl(console->sin_addr.s_addr), CLOSED_PORT);
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	const int connected = fd >= 0 ? connect(fd, (const struct sockaddr *)&closed, sizeof(closed)) : 0;
	const int connect_error = errno;
	if (fd >= 0)
		close(fd);
	xsdk_log_write(
		"net: a connection to the console's port %d: connect %d, errno %d", CLOSED_PORT, connected, connect_error);
	check(
		"net: a connection to a closed port is refused, ECONNREFUSED", connected < 0 && connect_error == ECONNREFUSED);

	// Without blocking, XAM says only that it failed, which SO_ERROR gives as EIO (src/socket.c).
	const int second = socket(AF_INET, SOCK_STREAM, 0);
	short events = 0;
	int error = 0;
	const bool answered = second >= 0 && connect_without_blocking(second, &closed, &events, &error);
	if (second >= 0)
		close(second);
	xsdk_log_write("net: without blocking: revents %04X, SO_ERROR %d", (unsigned)events, error);
	check("net: a refused connection without blocking gives POLLERR and an SO_ERROR",
		answered && (events & POLLERR) && error == EIO);
}

static void test_non_blocking(const Host *pc)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	short events = 0;
	int error = -1;
	const bool completed =
		fd >= 0 && connect_without_blocking(fd, &pc->address, &events, &error) && (events & POLLOUT) && error == 0;
	char nothing;
	const ssize_t received = completed ? recv(fd, &nothing, 1, 0) : 0;
	const bool would_block = received < 0 && errno == EAGAIN;
	check("net: a non-blocking connection completes through poll, and recv with nothing there gives EAGAIN",
		completed && would_block && echoes(fd, "tcp without blocking\n"));
	if (fd >= 0)
		close(fd);
}

static void test_many(void)
{
	int opened = 0;
	for (; opened < SOCKETS_IN_TURN; opened++) {
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			xsdk_log_write("net: socket %d of %d failed: %d", opened + 1, SOCKETS_IN_TURN, errno);
			break;
		}
		close(fd);
	}
	check("net: 1000 sockets opened and closed in turn", opened == SOCKETS_IN_TURN);
}

void test_net(void)
{
	Host pc;
	const bool known = read_host(&pc);
	check("net: net_host.txt names the PC", known);
	if (!known)
		return;
	struct sockaddr_in console = { 0 };
	test_tcp(&pc, &console);
	test_udp(&pc);
	test_accept(&pc);
	test_names();
	test_refused(&console);
	test_non_blocking(&pc);
	test_many();
}
