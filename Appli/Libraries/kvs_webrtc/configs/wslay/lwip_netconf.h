/*
 * lwip_netconf.h — Ameba Pro2 lwIP network config shim for STM32 target.
 *
 * Ameba's lwIP port exposes LwIP_GetIP(if_idx) to retrieve a pointer to the
 * raw IPv4 address bytes of a given interface.  On STM32 we map this to
 * lwIP's netif_default.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LWIP_NETCONF_SHIM_H
#define LWIP_NETCONF_SHIM_H

#include <stdint.h>
#include "lwip/netif.h"


/**
 * Return a pointer to the 4-byte network-order IPv4 address of interface
 * @p if_idx (0 = default interface).
 */
#define LwIP_GetIP( if_idx )    ( ( uint8_t * ) ( &( netif_default->ip_addr ) ) )

#endif /* LWIP_NETCONF_SHIM_H */
