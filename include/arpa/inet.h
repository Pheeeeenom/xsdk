#ifndef XSDK_ARPA_INET_H
#define XSDK_ARPA_INET_H

#include <netinet/in.h>

// IPv4 addresses as text and back (src/netdb.c).

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 for a dotted IPv4 address, 0 for other text, and -1 with EAFNOSUPPORT for any family but AF_INET.
int inet_pton(int family, const char *text, void *address);
const char *inet_ntop(int family, const void *address, char *text, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif
