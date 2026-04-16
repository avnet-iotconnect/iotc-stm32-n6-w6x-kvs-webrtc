/*
 * STM32N6570 KVS WebRTC port — media_cam.h
 *
 * DCMIPP camera initialisation and frame-capture interface.
 * Derived from x-cube-n6-ai-h264-usb-uvc/Inc/app_cam.h with the AI/NPU
 * and USB-UVC paths removed.
 *
 * Copyright (c) 2023 STMicroelectronics.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#ifndef MEDIA_CAM_H
#define MEDIA_CAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CAMERA_FPS — capture/encode framerate.
 *
 * Successively reduced to trade frame-rate for PSRAM/AXI headroom:
 *   30 → 15 → 10.  At 30 fps the XSPI1 PSRAM bus was contended by
 *   DCMIPP writes + VENC reads + Wi-Fi/lwIP traffic and would deadlock
 *   the AXI matrix shortly after camera DMA started, taking the entire
 *   scheduler down with it (no Hard/Bus/Mem fault printed — just a hard
 *   memory-bus stall).
 *
 *   At 15 fps we observed DCMIPP pipe-1 OVR (overrun) events which left
 *   the HAL pipe state in ERROR with the OVR interrupt permanently
 *   disabled — diagnostic log showed OVR arriving on/around the moment
 *   the encoder+network pipeline was at peak bandwidth demand.  Lowering
 *   to 10 fps reduces sustained DCMIPP write bandwidth another 33%
 *   (~24 MB/s) and gives the encoder/network stack more slack between
 *   frames.  Combined with the new OVR recovery path in
 *   CMW_CAMERA_PIPE_ErrorCallback, this should make the pipeline
 *   resilient to transient AXI contention spikes.                      */
/* Attempted 10 → 5 on 2026-04-14 to shrink per-frame size for the TURN+TCP
 * pipe, but the IMX335 CMW driver rejects any framerate not in
 * {10,15,20,25,30} (cmw_imx335.c:149 available_imx335_fps[]) with
 * CMW_ERROR_WRONG_PARAM.  CMW_CAMERA_Init returned -7 ("r=9" in the
 * [CAM] S3 Init< trace), MediaCam_Init asserted, and the KVS task never
 * registered as master so IoTConnect's Start Video button was a no-op.
 *
 * Held at 10 fps (sensor minimum) and rely on the encoder bitrate cap
 * (300 kbps — see media_enc.c:280) to fit the TURN+TCP pipe.  At
 * 300 kbps/10 fps the average frame is ~3.75 KB, still well inside the
 * 500 ms socket-mutex window.
 *
 * 2026-04-16: raised to 15 fps.  V1.3.0 middleware fixed WAN UDP RX so
 * ICE now negotiates UDP TURN relay.  2 Mbps / 15 fps confirmed stable
 * at 640x480.  NV12 at 640x480×1.5B×15fps = 6.9 MB/s DCMIPP writes —
 * well under the ~20 MB/s where OVR was previously observed at 720p.    */
#define CAMERA_FPS  15

/*
 * Initialise the DCMIPP camera pipeline (pipe 1 only — encoder path).
 * Must be called once before MediaCam_Start().
 */
void MediaCam_Init( void );

/*
 * Start continuous capture into the provided frame buffer.
 * cam_mode: CMW_MODE_SNAPSHOT or CMW_MODE_CONTINUOUS (pass CMW_MODE_CONTINUOUS).
 */
void MediaCam_Start( uint8_t * pFrameBuffer, uint32_t ulCamMode );

/*
 * A just-completed NV12 frame — Y-plane + interleaved UV-plane base addresses.
 */
typedef struct
{
    uint8_t * pY;
    uint8_t * pUV;
} MediaCamFrame_t;

/*
 * Start capture in DCMIPP double-buffer ("ping-pong") mode with NV12 semi-
 * planar output.
 *
 * DCMIPP hardware writes alternately to the (pY0, pUV0) pair and the
 * (pY1, pUV1) pair on each VSYNC: the Y plane goes to P1PPM0AR1/AR2 and
 * the interleaved UV plane to P1PPM1AR1/AR2.  The caller must drain
 * completed frames via MediaCam_WaitFrame() and finish reading each
 * returned pair before the next frame-complete event, otherwise DCMIPP
 * will begin overwriting the buffers still being read.
 *
 * All four pointers must be aligned on a 16-byte boundary.
 */
void MediaCam_StartDouble( uint8_t * pY0, uint8_t * pUV0,
                           uint8_t * pY1, uint8_t * pUV1,
                           uint32_t  ulCamMode );

/*
 * Block until DCMIPP signals end-of-frame and return the Y/UV pair for the
 * buffer that has just been fully written (the one that is NOT currently
 * being written by DCMIPP).  Safe to call only after MediaCam_StartDouble().
 */
MediaCamFrame_t MediaCam_WaitFrame( void );

/*
 * Stop the DCMIPP camera pipeline and put the sensor in standby.
 * Must be called before the media task exits so that the next
 * MediaCam_StartDouble() finds the pipe in READY state.  Safe to call
 * even if the pipe is already stopped or in ERROR state.
 */
void MediaCam_Stop( void );

/*
 * Must be called periodically from the media task to let the ISP update
 * its auto-exposure / auto-white-balance state.
 */
void MediaCam_IspUpdate( void );

/* Return the encoder-path output dimensions (set after MediaCam_Init). */
int  MediaCam_GetWidth( void );
int  MediaCam_GetHeight( void );

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_CAM_H */
