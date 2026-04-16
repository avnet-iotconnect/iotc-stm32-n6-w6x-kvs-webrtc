/*
 * STM32N6570 KVS WebRTC port — kvs_webrtc_task.h
 *
 * FreeRTOS task that runs the KVS WebRTC master peer, streaming H.264
 * video from the on-board DCMIPP+HW-encoder pipeline to AWS KVS viewers.
 * Runs in parallel with the existing IOTCONNECT MQTT agent task.
 *
 * Copyright (c) 2025 STMicroelectronics / project contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KVS_WEBRTC_TASK_H
#define KVS_WEBRTC_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FreeRTOS task entry point.
 *
 * Creates internally:
 *   - KVSMedia capture+encode task (priority 3)
 *   - KVS WebRTC master signaling loop (runs in this task context)
 *
 * Waits for EVT_MASK_NET_CONNECTED before attempting any network I/O.
 * Retries with exponential backoff on connection failure.
 * Pets the IWDG watchdog throughout.
 */
void vKvsWebRtcTask( void * pvParameters );

#ifdef __cplusplus
}
#endif

#endif /* KVS_WEBRTC_TASK_H */
