/**
  ******************************************************************************
  * @file    net_diag.h
  * @brief   WAN reachability diagnostic for ST67W611M bug report.
  *
  * Dumps STA netmask/IP/gateway and fires ICMP echo probes at a handful of
  * WAN targets so the captured UART log can be attached to the ST bug
  * report at _st_bugreport_artifacts/.  See net_diag.c for the discriminator
  * logic (protocol-specific UDP filter vs global routing/ACL drop).
  ******************************************************************************
  */

#ifndef NET_DIAG_H
#define NET_DIAG_H

#include "lwip/netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the full reachability diagnostic on the given STA netif.
 * Output goes to raw UART with the [NETDIAG] prefix (bypasses FreeRTOS
 * logging and LwIP).  Blocks for up to ~4 × 3 s = 12 s on timeouts.      */
void NetDiag_Run( struct netif *netif );

#ifdef __cplusplus
}
#endif

#endif /* NET_DIAG_H */
