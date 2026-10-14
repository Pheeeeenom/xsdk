#ifndef XSDK_NETINET_IN_H
#define XSDK_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

// IPv4 addresses, laid out as Winsock's (xenia:src/xenia/kernel/xsocket.h:63-67).

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
	in_addr_t s_addr;
};

struct sockaddr_in {
	sa_family_t sin_family;
	in_port_t sin_port;
	struct in_addr sin_addr;
	unsigned char sin_zero[8];
};

enum {
	IPPROTO_IP = 0,
	IPPROTO_TCP = 6,
	IPPROTO_UDP = 17,
	INET_ADDRSTRLEN = 16,
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFF)

// The console is big-endian, as the network is, so these change nothing.
static inline uint32_t htonl(uint32_t value)
{
	return value;
}

static inline uint16_t htons(uint16_t value)
{
	return value;
}

static inline uint32_t ntohl(uint32_t value)
{
	return value;
}

static inline uint16_t ntohs(uint16_t value)
{
	return value;
}

#ifdef __cplusplus
}
#endif

#endif
