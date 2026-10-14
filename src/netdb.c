#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Names to IPv4 addresses through XAM's DNS (xenia:src/xenia/kernel/xam/xam_net.cc:542-575), which resolves names on
// the console (docs/HARDWARE.md, "Sockets reach the LAN and the internet when marked insecure").

enum {
	ADDRESS_PARTS = 4,
	PART_MAX = 255,
	PART_DIGITS = 3,
	PORT_MAX = 65535,
	// How long a lookup may take before getaddrinfo gives up on it.
	DNS_TIMEOUT_MS = 30000,
	HUNDRED_NANOSECONDS_PER_MILLISECOND = 10000,
	// Winsock's, which an XNDNS's status holds when a name isn't found or the lookup should be tried again.
	WSAHOST_NOT_FOUND = 11001,
	WSATRY_AGAIN = 11002,
};

static const int KNOWN_FLAGS = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV;

int inet_pton(int family, const char *text, void *address)
{
	if (family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	uint32_t value = 0;
	for (int part = 0; part < ADDRESS_PARTS; part++) {
		if (part > 0 && *text++ != '.')
			return 0;
		uint32_t number = 0;
		int digits = 0;
		while (*text >= '0' && *text <= '9' && digits < PART_DIGITS) {
			number = number * 10 + (uint32_t)(*text++ - '0');
			digits++;
		}
		if (digits == 0 || number > PART_MAX)
			return 0;
		value = value << 8 | number;
	}
	if (*text != 0)
		return 0;
	// Network order is the console's own.
	memcpy(address, &value, sizeof(value));
	return 1;
}

const char *inet_ntop(int family, const void *address, char *text, socklen_t size)
{
	if (family != AF_INET) {
		errno = EAFNOSUPPORT;
		return NULL;
	}
	uint32_t value;
	memcpy(&value, address, sizeof(value));
	char formatted[INET_ADDRSTRLEN];
	const int length = snprintf(formatted, sizeof(formatted), "%u.%u.%u.%u", (unsigned)(value >> 24),
		(unsigned)(value >> 16 & 0xFF), (unsigned)(value >> 8 & 0xFF), (unsigned)(value & 0xFF));
	if ((socklen_t)length >= size) {
		errno = ENOSPC;
		return NULL;
	}
	memcpy(text, formatted, (size_t)length + 1);
	return text;
}

// A port number, or -1: services are numbers only.
static int32_t parse_port(const char *service)
{
	if (!service)
		return 0;
	char *end;
	const long port = strtol(service, &end, 10);
	return end != service && *end == 0 && port >= 0 && port <= PORT_MAX ? (int32_t)port : -1;
}

static int dns_error(int32_t status)
{
	switch (status) {
	case WSAHOST_NOT_FOUND:
		return EAI_NONAME;
	case WSATRY_AGAIN:
		return EAI_AGAIN;
	default:
		return EAI_FAIL;
	}
}

// Looks a name up; returns 0 with its addresses, or an EAI_ value.
static int look_up(const char *name, uint32_t addresses[XNET_DNS_ADDRESSES], uint32_t *count)
{
	const int error = xsdk_network_start();
	if (error) {
		errno = error;
		return EAI_SYSTEM;
	}
	HANDLE done;
	NTSTATUS status = NtCreateEvent(&done, NULL, NOTIFICATION_EVENT, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("getaddrinfo: NtCreateEvent failed: 0x%08X", (unsigned)status);
		return EAI_MEMORY;
	}
	XNDNS *result = NULL;
	const int32_t started = NetDll_XNetDnsLookup(XNCALLER_TITLE, name, done, &result);
	if (started != 0 || !result) {
		xsdk_log_write("getaddrinfo: XNetDnsLookup(%s) failed to start: %d", name, (int)started);
		(void)NtClose(done);
		return EAI_FAIL;
	}
	const int64_t timeout = -(int64_t)DNS_TIMEOUT_MS * HUNDRED_NANOSECONDS_PER_MILLISECOND;
	status = NtWaitForSingleObjectEx(done, USER_MODE, 0, &timeout);
	(void)NtClose(done);
	int outcome = 0;
	if (status == STATUS_TIMEOUT) {
		xsdk_log_write("getaddrinfo: no answer for %s in %u ms", name, (unsigned)DNS_TIMEOUT_MS);
		outcome = EAI_AGAIN;
	} else if (result->status != 0 || result->count == 0)
		outcome = dns_error(result->status);
	else {
		*count = result->count < XNET_DNS_ADDRESSES ? result->count : XNET_DNS_ADDRESSES;
		memcpy(addresses, result->addresses, *count * sizeof(addresses[0]));
	}
	(void)NetDll_XNetDnsRelease(XNCALLER_TITLE, result);
	return outcome;
}

static struct addrinfo *new_entry(uint32_t address, uint16_t port, int type)
{
	struct {
		struct addrinfo info;
		struct sockaddr_in address;
	} *entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->address = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = port, .sin_addr = { address } };
	entry->info = (struct addrinfo){ .ai_family = AF_INET,
		.ai_socktype = type,
		.ai_protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP,
		.ai_addrlen = sizeof(struct sockaddr_in),
		.ai_addr = (struct sockaddr *)&entry->address };
	return &entry->info;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
	const struct addrinfo none = { 0 };
	if (!hints)
		hints = &none;
	if (hints->ai_flags & ~KNOWN_FLAGS)
		return EAI_BADFLAGS;
	if (hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
		return EAI_FAMILY;
	if (hints->ai_socktype != 0 && hints->ai_socktype != SOCK_STREAM && hints->ai_socktype != SOCK_DGRAM)
		return EAI_SOCKTYPE;
	const int32_t port = parse_port(service);
	if (port < 0)
		return EAI_SERVICE;
	if (!node && !service)
		return EAI_NONAME;

	uint32_t addresses[XNET_DNS_ADDRESSES];
	uint32_t count = 1;
	if (!node)
		addresses[0] = hints->ai_flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK;
	else if (inet_pton(AF_INET, node, &addresses[0]) != 1) {
		if (hints->ai_flags & AI_NUMERICHOST)
			return EAI_NONAME;
		const int error = look_up(node, addresses, &count);
		if (error)
			return error;
	}

	// Every address, for each socket type asked for, or both.
	const int types[] = { SOCK_STREAM, SOCK_DGRAM };
	struct addrinfo *first = NULL;
	struct addrinfo **next = &first;
	for (uint32_t a = 0; a < count; a++) {
		for (int t = 0; t < 2; t++) {
			if (hints->ai_socktype != 0 && hints->ai_socktype != types[t])
				continue;
			struct addrinfo *entry = new_entry(addresses[a], (uint16_t)port, types[t]);
			if (!entry) {
				freeaddrinfo(first);
				return EAI_MEMORY;
			}
			*next = entry;
			next = &entry->ai_next;
		}
	}
	if (hints->ai_flags & AI_CANONNAME && node) {
		// XAM gives no canonical name, so the name asked for stands in.
		first->ai_canonname = strdup(node);
		if (!first->ai_canonname) {
			freeaddrinfo(first);
			return EAI_MEMORY;
		}
	}
	*result = first;
	return 0;
}

void freeaddrinfo(struct addrinfo *list)
{
	while (list) {
		struct addrinfo *next = list->ai_next;
		free(list->ai_canonname);
		free(list);
		list = next;
	}
}

const char *gai_strerror(int error)
{
	switch (error) {
	case 0:
		return "Success";
	case EAI_BADFLAGS:
		return "Invalid flags";
	case EAI_NONAME:
		return "Name not known";
	case EAI_AGAIN:
		return "Temporary failure in name resolution";
	case EAI_FAIL:
		return "Name resolution failed";
	case EAI_FAMILY:
		return "Address family not supported";
	case EAI_SOCKTYPE:
		return "Socket type not supported";
	case EAI_SERVICE:
		return "Service not supported";
	case EAI_MEMORY:
		return "Out of memory";
	case EAI_SYSTEM:
		return "System error";
	default:
		return "Unknown error";
	}
}
