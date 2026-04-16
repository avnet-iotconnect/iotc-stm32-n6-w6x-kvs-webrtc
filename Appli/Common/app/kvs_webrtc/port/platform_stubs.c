/**
 * platform_stubs.c — STM32N6570-DK stubs for Ameba-specific and POSIX
 *                    functions called by the KVS WebRTC SDK.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>

/* --------------------------------------------------------------------------
 * clock_gettime — derive wall-clock time from _gettimeofday().
 * newlib for arm-none-eabi does not provide this; sntp_port.c and
 * transport_dtls_mbedtls.c call it.  The SNTP stack updates the underlying
 * _gettimeofday stub when it synchronises.
 * -------------------------------------------------------------------------- */
#ifndef CLOCK_REALTIME
#  define CLOCK_REALTIME 0
#endif

/* Declare _gettimeofday as provided by syscalls.c */
extern int _gettimeofday( struct timeval *tv, void *tz );

int clock_gettime( int clk_id, struct timespec *tp )
{
    ( void ) clk_id;
    if( tp != NULL )
    {
        struct timeval tv;
        _gettimeofday( &tv, NULL );
        tp->tv_sec  = tv.tv_sec;
        tp->tv_nsec = ( long ) tv.tv_usec * 1000L;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * crypto_init / platform_set_malloc_free — Ameba-RTK platform hooks.
 * On STM32 the crypto hardware and heap are already initialised by the HAL
 * and newlib; these functions are no-ops.
 * -------------------------------------------------------------------------- */
int crypto_init( void )
{
    return 0;
}

int platform_set_malloc_free( void * ( *malloc_func )( size_t ),
                               void ( *free_func )( void * ) )
{
    ( void ) malloc_func;
    ( void ) free_func;
    return 0;
}

/* --------------------------------------------------------------------------
 * mbedtls_aes_crypt_ctr — AES-CTR mode not provided by the hardware AES
 * alternate implementation (MBEDTLS_AES_ALT).  Implement it here by calling
 * mbedtls_aes_crypt_ecb, which the hardware AES *does* provide.
 *
 * Marked weak so it is overridden by the real mbedTLS library in SW_Crypto
 * builds that link the full software implementation.
 * -------------------------------------------------------------------------- */
#include "mbedtls/aes.h"

__attribute__((weak))
int mbedtls_aes_crypt_ctr( mbedtls_aes_context *ctx,
                            size_t length,
                            size_t *nc_off,
                            unsigned char nonce_counter[ 16 ],
                            unsigned char stream_block[ 16 ],
                            const unsigned char *input,
                            unsigned char *output )
{
    int ret;
    int c;
    int i;
    size_t n;

    if( ctx == NULL || nc_off == NULL || nonce_counter == NULL ||
        stream_block == NULL || input == NULL || output == NULL )
        return( -1 );

    n = *nc_off;
    if( n > 0x0F )
        return( -1 );

    while( length-- )
    {
        if( n == 0 )
        {
            ret = mbedtls_aes_crypt_ecb( ctx, MBEDTLS_AES_ENCRYPT,
                                         nonce_counter, stream_block );
            if( ret != 0 )
                return( ret );
            for( i = 16; i > 0; i-- )
                if( ++nonce_counter[ i - 1 ] != 0 )
                    break;
        }
        c = *input++;
        *output++ = ( unsigned char ) ( c ^ stream_block[ n ] );
        n = ( n + 1 ) & 0x0F;
    }

    *nc_off = n;
    return( 0 );
}

/* --------------------------------------------------------------------------
 * mbedtls_ssl_conf_dtls_cookies — disable DTLS cookie verification.
 * Defined in ssl_srv.c only when MBEDTLS_SSL_SRV_C is enabled.  The KVS
 * DTLS transport calls this with NULL arguments to disable the feature; we
 * provide the stub so the linker is satisfied without enabling the full
 * TLS server module.
 *
 * Marked weak so it is overridden by ssl_srv.c in SW_Crypto builds.
 * -------------------------------------------------------------------------- */
#include "mbedtls/ssl.h"

__attribute__((weak))
void mbedtls_ssl_conf_dtls_cookies( mbedtls_ssl_config *conf,
                                    mbedtls_ssl_cookie_write_t *f_cookie_write,
                                    mbedtls_ssl_cookie_check_t *f_cookie_check,
                                    void *p_cookie )
{
    ( void ) conf;
    ( void ) f_cookie_write;
    ( void ) f_cookie_check;
    ( void ) p_cookie;
    /* No-op: cookie verification disabled */
}
