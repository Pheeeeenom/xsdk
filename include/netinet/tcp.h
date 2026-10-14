#ifndef XSDK_NETINET_TCP_H
#define XSDK_NETINET_TCP_H

// Winsock's TCP option, at level IPPROTO_TCP; it passes through to XAM (src/socket.c).
enum {
	TCP_NODELAY = 0x0001,
};

#endif
