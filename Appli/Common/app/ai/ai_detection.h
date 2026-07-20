/*
 * ai_detection.h — NPU object detection running alongside KVS WebRTC
 * streaming (DCMIPP Pipe2 -> ATON NPU -> IOTCONNECT telemetry).
 *
 * Entire feature is compiled out unless ENABLE_AI_DETECTION is defined.
 * See docs/ai_streaming_integration_plan.md.
 */
#ifndef AI_DETECTION_H
#define AI_DETECTION_H

#include <stdint.h>

#if defined( ENABLE_AI_DETECTION )

/* One-time init: NPU clocks/RAMs/cache/RIF + inference task creation.
 * Call after HW init, before the camera pipeline starts. */
void AiDetection_Init( void );

/* Configure DCMIPP PIPE2 for the model input (call from media_cam init,
 * after the PIPE1 config, with the sensor dimensions). */
void AiDetection_PipeInit( int lSensorWidth, int lSensorHeight );

/* Start/stop PIPE2 capture (call next to the PIPE1 start/stop). */
void AiDetection_PipeStart( void );
void AiDetection_PipeStop( void );

/* PIPE2 vsync/frame-done hook — call from the DCMIPP PIPE2 frame event
 * callback. Rotates capture buffers and wakes the inference task. */
void AiDetection_FrameDoneISR( void );

/* Telemetry snapshot (Phase 3): people count, best confidence (percent),
 * last inference duration (ms), total inference count.  Returns 0 until
 * the network is initialised (or when AI is compiled out); any output
 * pointer may be NULL. */
uint8_t AiDetection_GetTelemetry( int32_t * plDetections,
                                  uint32_t * pulTopConfPct,
                                  uint32_t * pulInferMs,
                                  uint32_t * pulInferences );

#else /* !ENABLE_AI_DETECTION */

#define AiDetection_Init()                    do {} while( 0 )
#define AiDetection_PipeInit( w, h )          do { ( void ) ( w ); ( void ) ( h ); } while( 0 )
#define AiDetection_PipeStart()               do {} while( 0 )
#define AiDetection_PipeStop()                do {} while( 0 )
#define AiDetection_FrameDoneISR()            do {} while( 0 )
#define AiDetection_GetTelemetry( d, c, m, n )    ( ( uint8_t ) 0U )

#endif /* ENABLE_AI_DETECTION */

#endif /* AI_DETECTION_H */
