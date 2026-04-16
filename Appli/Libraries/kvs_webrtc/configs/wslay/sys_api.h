/*
 * sys_api.h — Ameba Pro2 system API stub for STM32 target.
 *
 * Ameba's sys_api.h provides sys_backtrace_enable() and similar system
 * utilities.  On STM32 these are either handled by the platform or not needed;
 * stub them to no-ops so KVS SDK sources compile cleanly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SYS_API_STM32_SHIM_H
#define SYS_API_STM32_SHIM_H

#include <stdint.h>

/* Enable stack-unwind backtrace on exception — no-op on STM32. */
static inline void sys_backtrace_enable( void ) { }

#endif /* SYS_API_STM32_SHIM_H */
