#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// What a title's sockets reach on the console, through XAM's exports found by ordinal
// (xenia:src/xenia/kernel/xam/xam_table.inc:15-75), before xsdk has sockets of its own (docs/scope/networking.md).
// probe_net_host.txt beside the .xex holds the PC's address, the port tools/net_echo.py listens on there, and the flags
// XNetStartup takes: "192.168.0.125 7370 0". Each test runs on a plain socket, then on one marked with option 0x5801.

enum {
	// XNCALLER_TITLE (xenia:src/xenia/kernel/xam/xam_net.cc:42-47).
	CALLER_TITLE = 1,
	// Winsock's values, which XAM takes as they are: Xenia hands them to the host's sockets
	// (xenia:src/xenia/kernel/xsocket.h:73-91, xsocket.cc:99-101). FIONBIO and SO_ERROR are Winsock's, unverified here.
	NET_INET = 2,
	NET_STREAM = 1,
	NET_DATAGRAM = 2,
	NET_TCP = 6,
	NET_UDP = 17,
	NET_SOCKET_LEVEL = 0xFFFF,
	NET_SOCKET_ERROR_OPTION = 0x1007,
	NET_NON_BLOCKING = (int)0x8004667E,
	NET_WOULD_BLOCK = 10035,
	NET_IN_PROGRESS = 10036,
	NET_INVALID_SOCKET = -1,
	// Xenia takes either as disabling a socket's encryption (xenia:src/xenia/kernel/xsocket.cc:78-82).
	NET_MARK_INSECURE = 0x5801,
	// Winsock 2.2.
	NET_VERSION = 0x0202,
	// XNetStartupParams is 13 bytes (xenia:src/xenia/kernel/xam/xam_net.cc:154-168).
	STARTUP_PARAMS_BYTES = 13,
	// XNET_GET_XNADDR_PENDING and XNET_ETHERNET_LINK_ACTIVE (xam_net.cc:429-450, 529-535).
	ADDRESS_PENDING = 0,
	LINK_ACTIVE = 0x01,
	WSADATA_BYTES = 512,
	MAX_SOCKETS_IN_SET = 64,
	CONSOLE_LISTEN_PORT = 7371,
	CONNECT_TIMEOUT_MS = 3000,
	PUBLIC_CONNECT_TIMEOUT_MS = 5000,
	RECEIVE_TIMEOUT_MS = 3000,
	ACCEPT_TIMEOUT_MS = 5000,
	ADDRESS_TIMEOUT_MS = 10000,
	DNS_TIMEOUT_MS = 10000,
	POLL_MS = 50,
	MICROSECONDS_PER_MILLISECOND = 1000,
	LINE_BYTES = 128,
	HTTP_PORT = 80,
	HTTPS_PORT = 443,
};

// Each export the probe calls, by ordinal (xenia:src/xenia/kernel/xam/xam_table.inc).
enum {
	ORDINAL_WSA_STARTUP = 0x01,
	ORDINAL_SOCKET = 0x03,
	ORDINAL_CLOSE_SOCKET = 0x04,
	ORDINAL_IOCTL_SOCKET = 0x06,
	ORDINAL_SET_SOCKET_OPTION = 0x07,
	ORDINAL_GET_SOCKET_OPTION = 0x08,
	ORDINAL_BIND = 0x0B,
	ORDINAL_CONNECT = 0x0C,
	ORDINAL_LISTEN = 0x0D,
	ORDINAL_ACCEPT = 0x0E,
	ORDINAL_SELECT = 0x0F,
	ORDINAL_RECEIVE = 0x12,
	ORDINAL_RECEIVE_FROM = 0x14,
	ORDINAL_SEND = 0x16,
	ORDINAL_SEND_TO = 0x18,
	ORDINAL_LAST_ERROR = 0x1B,
	ORDINAL_XNET_STARTUP = 0x33,
	ORDINAL_DNS_LOOKUP = 0x43,
	ORDINAL_DNS_RELEASE = 0x44,
	ORDINAL_TITLE_ADDRESS = 0x49,
	ORDINAL_LINK_STATUS = 0x4B,
};

// sockaddr_in, big-endian as the console is (xenia:src/xenia/kernel/xsocket.h:63-67).
typedef struct {
	uint16_t family;
	uint16_t port;
	uint32_t address;
	uint8_t zero[8];
} NetAddress;

// XNADDR (xenia:src/xenia/kernel/xam/xam_net.cc:51-58).
typedef struct {
	uint32_t address;
	uint32_t online_address;
	uint16_t online_port;
	uint8_t ethernet[6];
	uint8_t online[20];
} TitleAddress;

// XNDNS (xam_net.cc:60-64).
typedef struct {
	int32_t status;
	uint32_t count;
	uint32_t addresses[8];
} DnsResult;

// Winsock's fd_set and timeval, as Xenia reads them (xam_net.cc:818-821).
typedef struct {
	uint32_t count;
	int32_t sockets[MAX_SOCKETS_IN_SET];
} SocketSet;

typedef struct {
	int32_t seconds;
	int32_t microseconds;
} WaitTime;

static struct {
	int (*wsa_startup)(uint32_t caller, uint16_t version, void *data);
	int32_t (*socket)(uint32_t caller, int family, int type, int protocol);
	int (*close_socket)(uint32_t caller, int32_t socket);
	int (*ioctl_socket)(uint32_t caller, int32_t socket, int command, uint32_t *argument);
	int (*set_socket_option)(uint32_t caller, int32_t socket, int level, int name, const void *value, int length);
	int (*get_socket_option)(uint32_t caller, int32_t socket, int level, int name, void *value, int *length);
	int (*bind)(uint32_t caller, int32_t socket, const NetAddress *address, int length);
	int (*connect)(uint32_t caller, int32_t socket, const NetAddress *address, int length);
	int (*listen)(uint32_t caller, int32_t socket, int backlog);
	int32_t (*accept)(uint32_t caller, int32_t socket, NetAddress *address, int *length);
	int (*select)(
		uint32_t caller, int count, SocketSet *read, SocketSet *write, SocketSet *error, const WaitTime *timeout);
	int (*receive)(uint32_t caller, int32_t socket, void *buffer, int length, int flags);
	int (*receive_from)(
		uint32_t caller, int32_t socket, void *buffer, int length, int flags, NetAddress *from, int *from_length);
	int (*send)(uint32_t caller, int32_t socket, const void *buffer, int length, int flags);
	int (*send_to)(uint32_t caller, int32_t socket, const void *buffer, int length, int flags, const NetAddress *to,
		int to_length);
	int (*last_error)(void);
	int (*xnet_startup)(uint32_t caller, const uint8_t *params);
	int (*dns_lookup)(uint32_t caller, const char *name, HANDLE event, DnsResult **result);
	int (*dns_release)(uint32_t caller, DnsResult *result);
	uint32_t (*title_address)(uint32_t caller, TitleAddress *address);
	uint32_t (*link_status)(uint32_t caller);
} xam;

typedef struct {
	void **slot;
	uint32_t ordinal;
	const char *name;
} Export;

static const Export exports[] = {
	{ (void **)&xam.wsa_startup, ORDINAL_WSA_STARTUP, "WSAStartup" },
	{ (void **)&xam.socket, ORDINAL_SOCKET, "socket" },
	{ (void **)&xam.close_socket, ORDINAL_CLOSE_SOCKET, "closesocket" },
	{ (void **)&xam.ioctl_socket, ORDINAL_IOCTL_SOCKET, "ioctlsocket" },
	{ (void **)&xam.set_socket_option, ORDINAL_SET_SOCKET_OPTION, "setsockopt" },
	{ (void **)&xam.get_socket_option, ORDINAL_GET_SOCKET_OPTION, "getsockopt" },
	{ (void **)&xam.bind, ORDINAL_BIND, "bind" },
	{ (void **)&xam.connect, ORDINAL_CONNECT, "connect" },
	{ (void **)&xam.listen, ORDINAL_LISTEN, "listen" },
	{ (void **)&xam.accept, ORDINAL_ACCEPT, "accept" },
	{ (void **)&xam.select, ORDINAL_SELECT, "select" },
	{ (void **)&xam.receive, ORDINAL_RECEIVE, "recv" },
	{ (void **)&xam.receive_from, ORDINAL_RECEIVE_FROM, "recvfrom" },
	{ (void **)&xam.send, ORDINAL_SEND, "send" },
	{ (void **)&xam.send_to, ORDINAL_SEND_TO, "sendto" },
	{ (void **)&xam.last_error, ORDINAL_LAST_ERROR, "WSAGetLastError" },
	{ (void **)&xam.xnet_startup, ORDINAL_XNET_STARTUP, "XNetStartup" },
	{ (void **)&xam.dns_lookup, ORDINAL_DNS_LOOKUP, "XNetDnsLookup" },
	{ (void **)&xam.dns_release, ORDINAL_DNS_RELEASE, "XNetDnsRelease" },
	{ (void **)&xam.title_address, ORDINAL_TITLE_ADDRESS, "XNetGetTitleXnAddr" },
	{ (void **)&xam.link_status, ORDINAL_LINK_STATUS, "XNetGetEthernetLinkStatus" },
};

static const char *const mode_names[] = { "plain", "marked 0x5801" };

static void address_text(uint32_t address, char text[16])
{
	snprintf(text, 16, "%u.%u.%u.%u", (unsigned)(address >> 24), (unsigned)(address >> 16 & 0xFF),
		(unsigned)(address >> 8 & 0xFF), (unsigned)(address & 0xFF));
}

static bool parse_address(const char *text, uint32_t *address)
{
	unsigned parts[4];
	if (sscanf(text, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4)
		return false;
	*address = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
	return true;
}

static NetAddress net_address(uint32_t address, uint16_t port)
{
	return (NetAddress){ .family = NET_INET, .port = port, .address = address };
}

static bool resolve_exports(void)
{
	HANDLE module = 0;
	const uint32_t error = XexGetModuleHandle("xam.xex", &module);
	if (error != 0) {
		xsdk_log_write("net: XexGetModuleHandle(xam.xex) failed: %08X", (unsigned)error);
		return false;
	}
	bool all = true;
	for (uint32_t i = 0; i < sizeof exports / sizeof exports[0]; i++) {
		if (XexGetProcedureAddress(module, exports[i].ordinal, exports[i].slot) < 0 || !*exports[i].slot) {
			xsdk_log_write("net: xam.xex has no ordinal 0x%02X (%s)", (unsigned)exports[i].ordinal, exports[i].name);
			all = false;
		}
	}
	return all;
}

// A socket of the type, marked insecure when the mode asks, and non-blocking; or NET_INVALID_SOCKET.
static int32_t open_socket(int type, int mode)
{
	const int32_t socket = xam.socket(CALLER_TITLE, NET_INET, type, type == NET_STREAM ? NET_TCP : NET_UDP);
	if (socket == NET_INVALID_SOCKET) {
		xsdk_log_write("net:   socket failed: %d", xam.last_error());
		return socket;
	}
	if (mode == 1) {
		const uint32_t on = 1;
		if (xam.set_socket_option(CALLER_TITLE, socket, NET_SOCKET_LEVEL, NET_MARK_INSECURE, &on, sizeof on) != 0)
			xsdk_log_write("net:   setsockopt 0x5801 failed: %d", xam.last_error());
	}
	uint32_t non_blocking = 1;
	if (xam.ioctl_socket(CALLER_TITLE, socket, NET_NON_BLOCKING, &non_blocking) != 0)
		xsdk_log_write("net:   FIONBIO failed: %d", xam.last_error());
	return socket;
}

// Waits for the socket to be readable, or writable, or to fail; returns whether it got there in time.
static bool wait_socket(int32_t socket, bool write, uint32_t timeout_ms)
{
	SocketSet wanted = { .count = 1, .sockets = { socket } };
	SocketSet failed = { .count = 1, .sockets = { socket } };
	const WaitTime timeout = { .seconds = (int32_t)(timeout_ms / 1000),
		.microseconds = (int32_t)(timeout_ms % 1000 * MICROSECONDS_PER_MILLISECOND) };
	const int ready = xam.select(CALLER_TITLE, 0, write ? 0 : &wanted, write ? &wanted : 0, &failed, &timeout);
	if (ready < 0) {
		xsdk_log_write("net:   select failed: %d", xam.last_error());
		return false;
	}
	return ready > 0 && wanted.count > 0;
}

static bool connect_socket(int32_t socket, uint32_t address, uint16_t port, uint32_t timeout_ms)
{
	const NetAddress to = net_address(address, port);
	if (xam.connect(CALLER_TITLE, socket, &to, sizeof to) == 0)
		return true;
	const int error = xam.last_error();
	if (error != NET_WOULD_BLOCK && error != NET_IN_PROGRESS) {
		xsdk_log_write("net:   connect failed: %d", error);
		return false;
	}
	if (!wait_socket(socket, true, timeout_ms)) {
		int socket_error = 0;
		int length = sizeof socket_error;
		(void)xam.get_socket_option(
			CALLER_TITLE, socket, NET_SOCKET_LEVEL, NET_SOCKET_ERROR_OPTION, &socket_error, &length);
		xsdk_log_write("net:   connect didn't complete within %u ms; SO_ERROR %d", (unsigned)timeout_ms, socket_error);
		return false;
	}
	return true;
}

// Receives what arrives within the timeout, NUL-terminated; returns its length, or -1.
static int receive_text(int32_t socket, char *buffer, int size, uint32_t timeout_ms)
{
	if (!wait_socket(socket, false, timeout_ms)) {
		xsdk_log_write("net:   nothing arrived in %u ms", (unsigned)timeout_ms);
		return -1;
	}
	const int received = xam.receive(CALLER_TITLE, socket, buffer, size - 1, 0);
	if (received < 0) {
		xsdk_log_write("net:   recv failed: %d", xam.last_error());
		return -1;
	}
	buffer[received] = 0;
	return received;
}

static bool send_text(int32_t socket, const char *text)
{
	const int length = (int)strlen(text);
	const int sent = xam.send(CALLER_TITLE, socket, text, length, 0);
	if (sent != length) {
		xsdk_log_write("net:   send gave %d of %d: %d", sent, length, xam.last_error());
		return false;
	}
	return true;
}

static void test_tcp_echo(int mode, uint32_t pc, uint16_t port)
{
	const int32_t socket = open_socket(NET_STREAM, mode);
	bool echoed = false;
	if (socket != NET_INVALID_SOCKET && connect_socket(socket, pc, port, CONNECT_TIMEOUT_MS)) {
		char line[LINE_BYTES];
		snprintf(line, sizeof line, "tcp echo, %s\n", mode_names[mode]);
		char reply[LINE_BYTES];
		echoed = send_text(socket, line) && receive_text(socket, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0 &&
			strcmp(reply, line) == 0;
	}
	if (socket != NET_INVALID_SOCKET)
		(void)xam.close_socket(CALLER_TITLE, socket);
	xsdk_log_write("net: %s: TCP to the PC and back: %s", mode_names[mode], echoed ? "yes" : "no");
}

static void test_udp_echo(int mode, uint32_t pc, uint16_t port)
{
	const int32_t socket = open_socket(NET_DATAGRAM, mode);
	bool echoed = false;
	if (socket != NET_INVALID_SOCKET) {
		char line[LINE_BYTES];
		snprintf(line, sizeof line, "udp echo, %s\n", mode_names[mode]);
		const NetAddress to = net_address(pc, port);
		const int length = (int)strlen(line);
		const int sent = xam.send_to(CALLER_TITLE, socket, line, length, 0, &to, sizeof to);
		if (sent != length)
			xsdk_log_write("net:   sendto gave %d of %d: %d", sent, length, xam.last_error());
		else if (wait_socket(socket, false, RECEIVE_TIMEOUT_MS)) {
			char reply[LINE_BYTES];
			NetAddress from;
			int from_length = sizeof from;
			const int received =
				xam.receive_from(CALLER_TITLE, socket, reply, sizeof reply - 1, 0, &from, &from_length);
			if (received >= 0) {
				reply[received] = 0;
				echoed = strcmp(reply, line) == 0;
			} else
				xsdk_log_write("net:   recvfrom failed: %d", xam.last_error());
		} else
			xsdk_log_write("net:   no datagram came back in %u ms", (unsigned)RECEIVE_TIMEOUT_MS);
		(void)xam.close_socket(CALLER_TITLE, socket);
	}
	xsdk_log_write("net: %s: UDP to the PC and back: %s", mode_names[mode], echoed ? "yes" : "no");
}

// Listens, asks the PC over TCP to connect back, and takes its line.
static void test_accept(int mode, uint32_t pc, uint16_t port)
{
	const uint16_t listen_port = (uint16_t)(CONSOLE_LISTEN_PORT + mode);
	const int32_t listener = open_socket(NET_STREAM, mode);
	bool accepted = false;
	if (listener != NET_INVALID_SOCKET) {
		const NetAddress any = net_address(0, listen_port);
		if (xam.bind(CALLER_TITLE, listener, &any, sizeof any) != 0)
			xsdk_log_write("net:   bind failed: %d", xam.last_error());
		else if (xam.listen(CALLER_TITLE, listener, 1) != 0)
			xsdk_log_write("net:   listen failed: %d", xam.last_error());
		else {
			const int32_t asker = open_socket(NET_STREAM, mode);
			char request[LINE_BYTES];
			snprintf(request, sizeof request, "back %u\n", (unsigned)listen_port);
			char reply[LINE_BYTES];
			const bool asked = asker != NET_INVALID_SOCKET && connect_socket(asker, pc, port, CONNECT_TIMEOUT_MS) &&
				send_text(asker, request) && receive_text(asker, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0;
			if (asker != NET_INVALID_SOCKET)
				(void)xam.close_socket(CALLER_TITLE, asker);
			if (!asked)
				xsdk_log_write("net:   couldn't ask the PC to connect back");
			else if (wait_socket(listener, false, ACCEPT_TIMEOUT_MS)) {
				NetAddress from;
				int from_length = sizeof from;
				const int32_t connection = xam.accept(CALLER_TITLE, listener, &from, &from_length);
				if (connection == NET_INVALID_SOCKET)
					xsdk_log_write("net:   accept failed: %d", xam.last_error());
				else {
					char line[LINE_BYTES];
					accepted = receive_text(connection, line, sizeof line, RECEIVE_TIMEOUT_MS) > 0 &&
						strncmp(line, "hello from the PC", 17) == 0;
					(void)xam.close_socket(CALLER_TITLE, connection);
				}
			} else
				xsdk_log_write("net:   no connection came in %u ms", (unsigned)ACCEPT_TIMEOUT_MS);
		}
		(void)xam.close_socket(CALLER_TITLE, listener);
	}
	xsdk_log_write("net: %s: a connection from the PC, accepted: %s", mode_names[mode], accepted ? "yes" : "no");
}

static void test_public_connect(int mode, uint32_t address, uint16_t port, const char *what)
{
	const int32_t socket = open_socket(NET_STREAM, mode);
	const bool connected =
		socket != NET_INVALID_SOCKET && connect_socket(socket, address, port, PUBLIC_CONNECT_TIMEOUT_MS);
	if (socket != NET_INVALID_SOCKET)
		(void)xam.close_socket(CALLER_TITLE, socket);
	xsdk_log_write("net: %s: TCP to %s: %s", mode_names[mode], what, connected ? "connected" : "no");
}

// An HTTP/1.0 request to a public host on port 80; logs the reply's status line.
static void test_http(int mode, uint32_t address, const char *host)
{
	const int32_t socket = open_socket(NET_STREAM, mode);
	char reply[LINE_BYTES] = "";
	bool answered = false;
	if (socket != NET_INVALID_SOCKET && connect_socket(socket, address, HTTP_PORT, PUBLIC_CONNECT_TIMEOUT_MS)) {
		char request[LINE_BYTES];
		snprintf(request, sizeof request, "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
		answered = send_text(socket, request) && receive_text(socket, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0;
		char *end = strpbrk(reply, "\r\n");
		if (end)
			*end = 0;
	}
	if (socket != NET_INVALID_SOCKET)
		(void)xam.close_socket(CALLER_TITLE, socket);
	xsdk_log_write("net: %s: HTTP from %s: %s", mode_names[mode], host, answered ? reply : "no reply");
}

static bool look_up(const char *name, uint32_t *address)
{
	DnsResult *result = 0;
	const int status = xam.dns_lookup(CALLER_TITLE, name, 0, &result);
	if (status != 0 || !result) {
		xsdk_log_write("net: DNS lookup of %s failed to start: %d", name, status);
		return false;
	}
	uint32_t waited = 0;
	while (result->status == NET_IN_PROGRESS && waited < DNS_TIMEOUT_MS) {
		(void)xsdk_thread_sleep(POLL_MS);
		waited += POLL_MS;
	}
	const bool found = result->status == 0 && result->count > 0;
	if (found) {
		*address = result->addresses[0];
		char text[16];
		address_text(*address, text);
		xsdk_log_write(
			"net: DNS: %s is %s, %u addresses, in about %u ms", name, text, (unsigned)result->count, (unsigned)waited);
	} else
		xsdk_log_write("net: DNS: %s not found: status %d, %u addresses, after %u ms", name, (int)result->status,
			(unsigned)result->count, (unsigned)waited);
	(void)xam.dns_release(CALLER_TITLE, result);
	return found;
}

static void log_title_address(void)
{
	TitleAddress address;
	uint32_t status = ADDRESS_PENDING;
	uint32_t waited = 0;
	while (waited < ADDRESS_TIMEOUT_MS) {
		memset(&address, 0, sizeof address);
		status = xam.title_address(CALLER_TITLE, &address);
		if (status != ADDRESS_PENDING)
			break;
		(void)xsdk_thread_sleep(POLL_MS);
		waited += POLL_MS;
	}
	char text[16];
	address_text(address.address, text);
	const uint8_t *mac = address.ethernet;
	xsdk_log_write("net: title address: status %08X after %u ms, %s, MAC %02X:%02X:%02X:%02X:%02X:%02X",
		(unsigned)status, (unsigned)waited, text, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int main(void)
{
	xsdk_log_write("net probe");
	char pc_text[LINE_BYTES];
	unsigned port = 0;
	unsigned flags = 0;
	FILE *host = fopen("probe_net_host.txt", "r");
	uint32_t pc = 0;
	if (!host || fscanf(host, "%15s %u %u", pc_text, &port, &flags) != 3 || !parse_address(pc_text, &pc)) {
		xsdk_log_write("net: probe_net_host.txt beside the .xex must hold \"<PC address> <port> <XNetStartup flags>\"");
		return 1;
	}
	fclose(host);
	if (!resolve_exports())
		return 1;

	uint8_t params[STARTUP_PARAMS_BYTES] = { STARTUP_PARAMS_BYTES, (uint8_t)flags };
	xsdk_log_write("net: XNetStartup, flags %u: %d", flags, xam.xnet_startup(CALLER_TITLE, params));
	static uint8_t wsa_data[WSADATA_BYTES];
	xsdk_log_write("net: WSAStartup 2.2: %d", xam.wsa_startup(CALLER_TITLE, NET_VERSION, wsa_data));
	const uint32_t link = xam.link_status(CALLER_TITLE);
	xsdk_log_write("net: link status %08X, %s", (unsigned)link, link & LINK_ACTIVE ? "active" : "not active");
	log_title_address();
	xsdk_log_write("net: the PC at %s, port %u", pc_text, port);

	uint32_t public_address = 0;
	(void)parse_address("1.1.1.1", &public_address);
	for (int mode = 0; mode < 2; mode++) {
		test_tcp_echo(mode, pc, (uint16_t)port);
		test_udp_echo(mode, pc, (uint16_t)port);
		test_accept(mode, pc, (uint16_t)port);
		test_public_connect(mode, public_address, HTTPS_PORT, "1.1.1.1:443");
	}
	uint32_t found = 0;
	if (look_up("example.com", &found)) {
		for (int mode = 0; mode < 2; mode++)
			test_http(mode, found, "example.com");
	}
	xsdk_log_write("net probe: done");
	return 0;
}
