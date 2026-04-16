/*
 * netinet/in.h — POSIX compat shim for lwIP-based embedded targets.
 *
 * Provides htonl/htons/ntohl/ntohs (and struct sockaddr_in / in_addr)
 * by delegating to the lwIP socket layer.
 *
 * Used by libsrtp/srtp/srtp.c and wslay sources that #include <netinet/in.h>.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COMPAT_NETINET_IN_H
#define COMPAT_NETINET_IN_H

/* lwIP sockets compatibility layer provides:
 *   - struct in_addr, struct sockaddr_in
 *   - htons / htonl / ntohs / ntohl (via lwip_htons etc.)
 *   - INADDR_ANY, AF_INET, SOCK_DGRAM, IPPROTO_UDP, etc.
 */
#include <lwip/sockets.h>
#include <lwip/inet.h>

#endif /* COMPAT_NETINET_IN_H */
