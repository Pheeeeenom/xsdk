#ifndef XSDK_NETDB_H
#define XSDK_NETDB_H

#include <netinet/in.h>
#include <sys/socket.h>

// Host names to addresses through XAM's DNS (src/netdb.c), IPv4 only; services are port numbers.

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	socklen_t ai_addrlen;
	struct sockaddr *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};

enum {
	AI_PASSIVE = 0x0001,
	AI_CANONNAME = 0x0002,
	AI_NUMERICHOST = 0x0004,
	AI_NUMERICSERV = 0x0400,
	EAI_BADFLAGS = -1,
	EAI_NONAME = -2,
	EAI_AGAIN = -3,
	EAI_FAIL = -4,
	EAI_FAMILY = -6,
	EAI_SOCKTYPE = -7,
	EAI_SERVICE = -8,
	EAI_MEMORY = -10,
	EAI_SYSTEM = -11,
};

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result);
void freeaddrinfo(struct addrinfo *list);
const char *gai_strerror(int error);

#ifdef __cplusplus
}
#endif

#endif
