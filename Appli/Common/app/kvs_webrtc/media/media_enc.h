/*
 * STM32N6570 KVS WebRTC port — media_enc.h
 *
 * STM32N6 hardware H.264 encoder interface for the KVS WebRTC media pipeline.
 * Derived from x-cube-n6-ai-h264-usb-uvc/Inc/app_enc.h.
 *
 * Copyright (c) 2023 STMicroelectronics.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#ifndef MEDIA_ENC_H
#define MEDIA_ENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int lWidth;
    int lHeight;
    int lFps;
} MediaEncConf_t;

/*
 * Initialise the H.264 hardware encoder.
 * Must be called once after MediaCam_Init().
 */
void MediaEnc_Init( const MediaEncConf_t * pxConf );

/* Release encoder resources. */
void MediaEnc_DeInit( void );

/*
 * Encode one raw frame.
 *
 * pInY          : pointer to the Y-plane of the NV12 frame from DCMIPP PIPE1
 * pInUV         : pointer to the interleaved UV-plane (NV12) — immediately
 *                 follows the Y-plane in memory but is addressed separately
 *                 because DCMIPP writes the two halves to two independent
 *                 bus addresses (P1PPM0AR1 / P1PPM1AR1).
 * pOut          : destination buffer for the H.264 NAL stream
 * xOutLen       : size of the output buffer in bytes
 * lIntraForce   : non-zero to force an IDR (intra) frame
 *
 * Returns the number of bytes written to pOut, or -1 on error.
 */
int MediaEnc_EncodeFrame( uint8_t * pInY,
                          uint8_t * pInUV,
                          uint8_t * pOut,
                          size_t    xOutLen,
                          int       lIntraForce );

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_ENC_H */
