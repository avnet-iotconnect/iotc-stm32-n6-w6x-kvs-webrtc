/*
 * STM32N6570 KVS WebRTC port — media_cam_config.h
 *
 * Sensor and encoder dimension configuration.
 * Derived from x-cube-n6-ai-h264-usb-uvc/Inc/app_config.h.
 * CAPTURE_FORMAT and CAPTURE_BPP must match what the H.264 encoder expects
 * (H264ENC_RGB888 input → 4 bytes/pixel, ARGB8888 layout).
 *
 * Copyright (c) 2023 STMicroelectronics.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#ifndef MEDIA_CAM_CONFIG_H
#define MEDIA_CAM_CONFIG_H

#include "cmw_camera.h"       /* DCMIPP_PIPE1, CMW_Aspect_ratio_manual_roi  */
#include "stm32n6xx_hal.h"    /* HAL_DCMIPP_SetIPPlugConfig etc.             */

/* ── Sensor native resolutions ──────────────────────────────────────────── */
#define SENSOR_IMX335_WIDTH   2592
#define SENSOR_IMX335_HEIGHT  1944
#define SENSOR_IMX335_FLIP    CMW_MIRRORFLIP_MIRROR

#define SENSOR_VD66GY_WIDTH   1120
#define SENSOR_VD66GY_HEIGHT   720
#define SENSOR_VD66GY_FLIP    CMW_MIRRORFLIP_FLIP

#define SENSOR_VD55G1_WIDTH    800
#define SENSOR_VD55G1_HEIGHT   600
#define SENSOR_VD55G1_FLIP    CMW_MIRRORFLIP_FLIP

#define SENSOR_VD1943_WIDTH      0   /* auto-detected by driver */
#define SENSOR_VD1943_HEIGHT     0
#define SENSOR_VD1943_FLIP    CMW_MIRRORFLIP_MIRROR

/* ── Encoder output resolutions (DCMIPP downscaler output) ──────────────── */
/* Dropped from 1280x720 → 640x480 to escape a VENC/AXI stall observed
 * after 4-5 frames: the log trace showed "[V] strmEnc>P" as the last
 * line before silence while DCMIPP, VENC and Wi-Fi all contended for
 * XSPI1/PSRAM bandwidth.  720p NV12 is 1.4 MB/frame × 10 fps = 13.5
 * MB/s of DCMIPP writes alone; VENC then reads those same planes and
 * writes its output bitstream back to PSRAM, and lwIP TX pbufs come
 * from the same XSPI1 pool.  At 640x480 the NV12 frame is 460 KB
 * (Y=307200 + UV=153600) so writes drop to ~4.6 MB/s, VENC reads also
 * drop by ~55%, and the encoded bitstream shrinks enough that a spike
 * I-frame no longer saturates the bus for tens of ms.
 *
 * 640 and 480 are both multiples of 16 (macroblock size), satisfying
 * H.264 encoder geometry requirements.                                    */
#define VENC_IMX335_WIDTH    640
#define VENC_IMX335_HEIGHT   480

#define VENC_VD66GY_WIDTH   1120
#define VENC_VD66GY_HEIGHT   720

#define VENC_VD55G1_WIDTH    640
#define VENC_VD55G1_HEIGHT   480

#define VENC_VD1943_WIDTH   1280
#define VENC_VD1943_HEIGHT   720

/* ── DCMIPP output pixel format fed to the H.264 encoder ────────────────── */
/* DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2 = NV12 semi-planar (Y plane + interleaved
 * UV plane).  At 12 bpp this is 4× less DCMIPP write bandwidth than the
 * previous ARGB8888 (32 bpp) path.
 *
 * Earlier ARGB8888 path was producing left-half-good / right-half-rainbow
 * stripe corruption: 1280×720×4B×15fps = 54 MB/s of sustained DCMIPP writes
 * into APS256XX PSRAM (XSPI1 at HSI 64 MHz), concurrent with VENC reads of
 * the same PSRAM region and lwIP TX bursts.  The XSPI1→PSRAM drain could
 * not keep up: DCMIPP DPREG overran, PIPE1 OVR flag fired, and the rightmost
 * ~half of each row was silently dropped.  Confirmed by a frame-row probe
 * showing byte-identical stale PSRAM content in the right columns across
 * multiple frames with alpha != 0xff.
 *
 * NV12 at 1280×720×1.5B×15fps = 13.5 MB/s fits comfortably inside the
 * PSRAM envelope.  The VENC input also drops from H264ENC_RGB888 (32 bpp
 * read, internal RGB→YUV conversion) to H264ENC_YUV420_SEMIPLANAR (native
 * YUV, no conversion step) — less read bandwidth and less HW work.
 *
 * CAPTURE_BPP is 1 here because DCMIPP's PixelPipePitch register programs
 * the Y-plane row pitch only (UV plane pitch equals Y pitch in NV12 —
 * 1280 bytes of interleaved UV per row, half as many UV rows).            */
#define CAPTURE_FORMAT   DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2
#define CAPTURE_BPP      1

#endif /* MEDIA_CAM_CONFIG_H */
