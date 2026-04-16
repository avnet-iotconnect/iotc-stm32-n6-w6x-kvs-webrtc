/*
 * mbedtls/config.h — compatibility shim for mbedTLS 3.x
 *
 * The KVS WebRTC SDK includes "mbedtls/config.h" (mbedTLS 2.x style).
 * mbedTLS 3.x renamed it to "mbedtls/build_info.h".  This shim forwards
 * to the project's actual mbedTLS 3.x header.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MBEDTLS_CONFIG_COMPAT_H
#define MBEDTLS_CONFIG_COMPAT_H

#include "mbedtls/build_info.h"

/* mbedTLS 2.x -> 3.x SRTP profile name aliases.
 * The KVS SDK uses the old MBEDTLS_SRTP_* names; mbedTLS 3.x renamed
 * them to MBEDTLS_TLS_SRTP_*.  These are lazily-expanded macros so they
 * resolve at point-of-use after mbedtls/ssl.h (which defines the _TLS_
 * variants) has been included.
 */
#ifndef MBEDTLS_SRTP_AES128_CM_HMAC_SHA1_80
#  define MBEDTLS_SRTP_AES128_CM_HMAC_SHA1_80  MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80
#endif
#ifndef MBEDTLS_SRTP_AES128_CM_HMAC_SHA1_32
#  define MBEDTLS_SRTP_AES128_CM_HMAC_SHA1_32  MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32
#endif
#ifndef MBEDTLS_SRTP_NULL_HMAC_SHA1_80
#  define MBEDTLS_SRTP_NULL_HMAC_SHA1_80        MBEDTLS_TLS_SRTP_NULL_HMAC_SHA1_80
#endif
#ifndef MBEDTLS_SRTP_NULL_HMAC_SHA1_32
#  define MBEDTLS_SRTP_NULL_HMAC_SHA1_32        MBEDTLS_TLS_SRTP_NULL_HMAC_SHA1_32
#endif

#endif /* MBEDTLS_CONFIG_COMPAT_H */
