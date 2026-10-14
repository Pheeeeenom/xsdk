# Networking

Agreed with the user on 2026-10-14, before milestone 36. A scope of its own, not a numbered milestone.

## What it is

Sockets for xsdk titles, and TLS 1.2 on top of them. The TLS runs in the title's own code, in software, over the
console's TCP. That is what "emulated" comes to here: the system's own secure layer plays no part in it.

## What first uses it

A homebrew shop: titles fetching over HTTP and HTTPS.

## What the console offers

- **Sockets.** XAM exports Winsock-style calls, `NetDll_socket` and the rest, by ordinal
  (xenia:src/xenia/kernel/xam/xam_table.inc:15-80). Each takes the caller's type first, `XNCALLER_TITLE` (1) for a
  title (xenia:src/xenia/kernel/xam/xam_net.cc:42-47). A title calls `NetDll_XNetStartup` (0x33) and
  `NetDll_WSAStartup` (0x01) before any of them. Xenia runs them on the host's sockets (xam_net.cc:643-1034).
- **The secure layer.** `NetDll_XNetStartup` takes a structure of byte-sized settings whose flags Xenia doesn't name
  (xam_net.cc:154-168). Xenia keeps each socket secure until a title sets `SOL_SOCKET` option 0x5801 or 0x5802 on it,
  which it calls disabling the socket's encryption (xenia:src/xenia/kernel/xsocket.cc:78-82). On the console, only
  sockets given option 0x5801 reach the LAN or the internet (step 1, below).
- **Names and the link.** `NetDll_XNetDnsLookup` (0x43), `NetDll_XNetGetEthernetLinkStatus` (0x4B) and
  `NetDll_XNetGetTitleXnAddr` are stubs in Xenia (xam_net.cc:476, 540, 560), so only the console can test them.
- **Random numbers.** xsdk's `getentropy` takes them from `XeCryptRandom` (src/entropy.c). Xenia fills the buffer with
  0xFD (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_crypt.cc:354-356), so TLS run there proves nothing about its keys.
- **The time.** xsdk's `clock_gettime(CLOCK_REALTIME)` reads `KeQuerySystemTime` (src/time.c), which certificate
  dates need.
- **What xsdk lacks.** xsdk has no sockets, and the toolchain's newlib has no `sys/socket.h`, `netinet/in.h` or
  `netdb.h`.

## Three steps, each run on the console

### 1. A probe: what reaches where

A probe title, tried with `NetDll_XNetStartup`'s flags at 0 and at 1, and each socket with and without option 0x5801.
It logs:

- the link status and the console's own address;
- a TCP connection to the PC, and the data an echo server there sends back;
- a UDP datagram to the PC, and its echo;
- a connection the PC makes to the console, accepted;
- a TCP connection to a public host's port 443, by address;
- a public host name looked up.

The echo server is a host tool, `tools/net_echo.py`. The PC's address reaches the console in a file beside the
`.xex`. HARDWARE.md records which settings reach the LAN and which reach the internet.

Done on 2026-10-14 (docs/HARDWARE.md, "Sockets reach the LAN and the internet when marked insecure"):
- plain sockets reached nothing;
- sockets given option 0x5801 reached the PC and the internet both ways, over TCP and UDP;
- DNS resolved names;
- the startup flags changed nothing.

### 2. Sockets in xsdk

- **The calls ports expect.** The POSIX headers and calls newlib-based ports expect, on XAM's exports:
  - `socket`, `bind`, `listen`, `accept`, `connect`, `shutdown`;
  - `send`, `recv`, `sendto`, `recvfrom`;
  - `setsockopt`, `getsockopt`, `getsockname`, `getpeername`;
  - `select`, `poll`, and `fcntl`'s `O_NONBLOCK`;
  - `getaddrinfo`, `freeaddrinfo`, `gai_strerror`, `inet_pton`, `inet_ntop`.
- **Sockets are descriptors.** A socket takes a number in xsdk's descriptor table, so `close`, `read` and `write` work
  on it.
- **Starting the network.** xsdk starts XAM's network on the first socket call, with no call from the title, and
  marks each socket with option 0x5801, as the probe found.
- **Errors.** XAM's error codes become `errno` values.

**Done when:**

- xsdk_test does each of these against `tools/net_echo.py` on the PC:
  - sends data over TCP and UDP and gets it back;
  - accepts a connection the PC makes;
  - looks up a public host name;
  - connects to a closed port and gets `ECONNREFUSED`;
  - completes a non-blocking connection through `poll`;
  - opens and closes more sockets in turn than XAM holds at once.
- Broken on purpose, `close` leaving the socket open, the last of those fails on the console.
- xsdk_test, vulkan4xenon_test and vkbench pass on the console.

### 3. TLS 1.2

- **A port of its own.** A TLS library ported as its own repository, as SDL is (CODE_GUIDELINES.md section 14). It's
  built with xsdk's toolchain on xsdk's sockets, `getentropy` and `clock_gettime`, so xsdk itself keeps to the
  toolchain (section 1). It's set up for TLS 1.2 only.
- **The library.** mbedTLS.
- **Certificates.** Root certificates come from a bundle in a file beside the title, so it can be updated without a
  rebuild: Mozilla's, as curl publishes it.
- **Speed.** It does its own cryptography at first. The handshake's time is measured, and the kernel's `XeCrypt`
  functions come in only if those numbers call for it.

**Done when:**

- The library's own self-tests pass on the console.
- A test title fetches a page over HTTPS from a public host with TLS 1.2. The chain is checked against the bundle and
  the host name against the certificate. The title logs the handshake's time, the cipher suite and the console's
  clock.
- Hosts with a wrong name, an expired certificate and a self-signed one are each refused. Broken on purpose, with the
  name check left out, the wrong-name case fails.
- Two runs log different handshake random values, as a generator stuck at one value would pass every other check.

## Out of scope

- TLS 1.3, and TLS servers.
- An HTTP library beyond the test title's one request. A curl port would be a scope of its own.
- Xbox Live, its secure layer and its VDP sockets. Nothing here signs in to Live or talks to its services.
- IPv6.

## Decided with the user

On 2026-10-14:
- **What first uses it:** a homebrew shop, with HTTP and HTTPS.
- **The TLS library:** mbedTLS.
- **The order:** before milestone 36 (docs/scope/36-texel-rate.md), while the user is at hand to restart the console.
- **Root certificates:** a file beside each title. The user left this open, and a shop wants it updated without a
  rebuild.
