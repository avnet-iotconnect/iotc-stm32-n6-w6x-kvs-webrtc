/*
 * npu_cache_stub.c — no-op NPU cache (CACHEAXI) shim for AI bring-up.
 *
 * The real npu_cache.c (Appli/Libraries/AI_Runtime/Npu/Devices/STM32N6xx)
 * needs the STM32N6 CACHEAXI HAL module, which this firmware's vendored HAL
 * copy does not ship.  Enabling it pulls a newer HAL header that clashes
 * with the rest of the tree.  The NPU AXI cache is a throughput
 * optimization, not a correctness requirement, so during bring-up we run
 * the NPU with its dedicated cache disabled and stub the API.
 *
 * TODO(perf): add the CACHEAXI HAL module cleanly and drop this stub —
 * expect a measurable inference-time reduction.  npu_cache.c is excluded
 * from the build in .cproject; this file provides the same symbols.
 */

#if defined( ENABLE_AI_DETECTION )

#include <stdint.h>
#include "npu_cache.h"

void npu_cache_enable( void )                                   { }
void npu_cache_disable( void )                                  { }
void npu_cache_invalidate( void )                               { }
void npu_cache_clean_range( uint32_t start, uint32_t end )
{
    ( void ) start; ( void ) end;
}
void npu_cache_clean_invalidate_range( uint32_t start, uint32_t end )
{
    ( void ) start; ( void ) end;
}
void npu_cache_enable_clocks_and_reset( void )                  { }
void npu_cache_disable_clocks_and_reset( void )                 { }

#endif /* ENABLE_AI_DETECTION */
