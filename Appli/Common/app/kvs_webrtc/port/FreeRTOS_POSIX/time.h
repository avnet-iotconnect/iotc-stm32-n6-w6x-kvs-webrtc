/*
 * STM32N6570 KVS WebRTC port — FreeRTOS_POSIX/time.h stub
 *
 * The KVS WebRTC SDK (core_http_helper.c) includes "FreeRTOS_POSIX/time.h"
 * which on the Ameba platform provides FreeRTOS-POSIX time types.  On STM32
 * with newlib the standard <time.h> already supplies struct timespec, time_t,
 * and clock_gettime, so this header simply re-exports it.
 *
 * Copyright (c) 2025 project contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FREERTOS_POSIX_TIME_H
#define FREERTOS_POSIX_TIME_H

/* Force newlib to expose POSIX timers (clock_gettime / CLOCK_REALTIME). */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <time.h>

/* If newlib still did not expose clock_gettime, provide the declaration
 * here so KVS SDK sources compile.  The implementation is in sntp_port.c
 * via the _gettimeofday() / clock_gettime() newlib weak stub chain. */
#ifndef __clock_gettime_declared
#  define __clock_gettime_declared 1
#  ifdef __cplusplus
extern "C" {
#  endif
#  ifndef CLOCK_REALTIME
#    define CLOCK_REALTIME 0
#  endif
int clock_gettime( int clk_id, struct timespec * tp );
#  ifdef __cplusplus
}
#  endif
#endif /* __clock_gettime_declared */

#endif /* FREERTOS_POSIX_TIME_H */
