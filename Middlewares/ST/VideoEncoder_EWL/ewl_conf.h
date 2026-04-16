/**
 * ewl_conf.h — EWL configuration for KVS WebRTC project on STM32N6570-DK.
 * Copied from ewl_conf_template.h and configured for FreeRTOS + malloc.
 */

#ifndef EWL_CONF_H
#define EWL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#define EWL_USE_MALLOC_MM     0
#define EWL_USE_FREERTOS_MM   1
#define EWL_USE_THREADX_MM    2
#define EWL_USE_STM32MPM_MM   3
#define EWL_USER_MM           4

/* Use standard malloc/free */
#define EWL_ALLOC_API EWL_USE_MALLOC_MM

#define EWL_USE_POLLING_SYNC  0
#define EWL_USE_FREERTOS_SYNC 1
#define EWL_USE_THREADX_SYNC  2
#define EWL_USER_SYNC         4

/* Use FreeRTOS IRQ-driven sync — matches reference project.
 * The VENC_IRQHandler has FUSE-bit clearing logic that the polling
 * path lacks, and IRQ-driven wait avoids busy-loop CPU burn. */
#define EWL_SYNC_API EWL_USE_FREERTOS_SYNC

#define ALIGNMENT_INCR 8UL
#define MEM_CHUNKS     32

#if (EWL_ALLOC_API == EWL_USE_MALLOC_MM)
#include <stdlib.h>
#endif

#if (EWL_ALLOC_API == EWL_USE_FREERTOS_MM) || (EWL_SYNC_API == EWL_USE_FREERTOS_SYNC)
#include "FreeRTOS.h"
#include "event_groups.h"
#endif

#define assert_param(expr) ((void)0U)

#ifdef __cplusplus
}
#endif

#endif /* EWL_CONF_H */
