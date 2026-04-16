/*
 * BackoffAlgorithm.h — name-alias shim for kvs_webrtc_task.c
 *
 * kvs_webrtc_task.c includes "BackoffAlgorithm.h" (FreeRTOS Labs naming
 * convention) while the library installed in this project is named
 * "backoff_algorithm.h" (AWS IoT Embedded C SDK convention).
 * This header simply re-exports the installed header.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BACKOFF_ALGORITHM_ALIAS_H
#define BACKOFF_ALGORITHM_ALIAS_H

#include "backoff_algorithm.h"

#endif /* BACKOFF_ALGORITHM_ALIAS_H */
