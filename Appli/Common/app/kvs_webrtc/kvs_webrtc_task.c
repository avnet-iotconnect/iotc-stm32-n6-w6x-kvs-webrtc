/*
 * STM32N6570 KVS WebRTC port — kvs_webrtc_task.c
 *
 * FreeRTOS task that runs the KVS WebRTC master peer.
 *
 * Flow:
 *   1. Wait for EVT_MASK_NET_CONNECTED (Wi-Fi + DHCP ready).
 *   2. Read KVS configuration from KVStore.
 *   3. Set runtime config globals consumed by app_common.c macros.
 *   4. Initialise AppCommon + AppMediaSource (camera+encoder pipeline).
 *   5. Run AppCommon_StartSignalingController() — blocks until disconnect.
 *   6. On failure / disconnect: retry with exponential backoff.
 *
 * Runs fully independently of the IOTCONNECT MQTT agent task.
 *
 * Copyright (c) 2025 STMicroelectronics / project contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

/* ── Includes ────────────────────────────────────────────────────────────── */

#include "kvs_webrtc_task.h"
#include "kvs_runtime_config.h"   /* must come before demo_config.h         */
#include "demo_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include "logging_levels.h"
#define LOG_LEVEL   LOG_INFO
#include "logging.h"

#include "sys_evt.h"              /* EVT_MASK_NET_CONNECTED, xSystemEvents   */
#include "kvstore.h"              /* KVStore_getStringHeap, CS_KVS_* */
#include "../ai/ai_detection.h"
#include "freertos_hooks.h"       /* vPetWatchdog()                          */

#include "BackoffAlgorithm.h"

/* PKCS#11 / PkiObject for loading device cert + key */
#include "PkiObject.h"
#include "tls_transport_config.h"   /* TLS_CERT_LABEL, TLS_KEY_PRV_LABEL   */
#include "mbedtls/pk.h"
#include "mbedtls/pem.h"
#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"

/* KVS WebRTC SDK application layer */
#include "app_common.h"
#include "app_media_source.h"

/* ST67W611M WAN reachability diagnostic for bug-report artifact. */
#include "net_diag.h"
#include "lwip.h"

#include <string.h>
#include <stdlib.h>

/* ── Retry / backoff configuration ─────────────────────────────────────── */
#define KVS_RETRY_BACKOFF_BASE_S       10U
#define KVS_RETRY_MAX_BACKOFF_S        300U   /* 5 minutes                  */
#define KVS_RETRY_BACKOFF_MULTIPLIER   1000U  /* base unit: milliseconds    */

/* ── Runtime config globals (extern-declared in kvs_runtime_config.h) ─── */
const char * pcKvsAwsRegion            = "";
const char * pcKvsChannelName          = "";
const char * pcKvsCredentialsEndpoint  = "";
const char * pcKvsIotThingName         = "";
const char * pcKvsIotRoleAlias         = "";
const char * pcKvsIotThingCert         = "";
const char * pcKvsIotPrivateKey        = "";

/* ── Private state ──────────────────────────────────────────────────────── */
/* ~1 MB — placed in external PSRAM (.psram_bss, NOLOAD) to free AXISRAM3-6
 * for the NPU activation pools (network.c bakes them at 0x34200000-0x343BFFF8).
 * PSRAM must be initialised (MediaPort_EnsurePsram) before first touch. */
static AppContext_t              xAppContext __attribute__( ( section( ".psram_bss" ), aligned( 32 ) ) );
static AppMediaSourcesContext_t  xAppMediaSourceContext;

/* KVStore heap strings — freed on each reconnect attempt */
static char * pcRegionBuf            = NULL;
static char * pcChannelBuf           = NULL;
static char * pcEndpointBuf          = NULL;
static char * pcThingNameBuf         = NULL;
static char * pcRoleAliasBuf         = NULL;
static char * pcThingCertBuf         = NULL;
static char * pcPrivateKeyBuf        = NULL;

/* PKCS#11-loaded PEM buffers — freed on each reconnect attempt */
static char * pcCertPemBuf           = NULL;
static char * pcKeyPemBuf            = NULL;

/* ── Forward declarations ───────────────────────────────────────────────── */
static int32_t prvInitTransceiver( void * pvMediaCtx,
                                   TransceiverTrackKind_t eTrackKind,
                                   Transceiver_t * pxTransceiver );
static int32_t prvOnMediaSinkHook( void * pvCustom,
                                   MediaFrame_t * pxFrame );
static void    prvFreeConfigBuffers( void );
static int     prvLoadConfigFromKVStore( void );
static int     prvLoadCertAndKeyPem( void );

/* ── Transceiver initialiser (video only) ───────────────────────────────── */

static int32_t prvInitTransceiver( void *                 pvMediaCtx,
                                   TransceiverTrackKind_t eTrackKind,
                                   Transceiver_t *        pxTransceiver )
{
    AppMediaSourcesContext_t * pxMediaCtx = ( AppMediaSourcesContext_t * ) pvMediaCtx;

    if( ( pvMediaCtx == NULL ) || ( pxTransceiver == NULL ) )
    {
        LogError( "[KVSWebRTC] NULL param in InitTransceiver." );
        return -1;
    }

    switch( eTrackKind )
    {
        case TRANSCEIVER_TRACK_KIND_VIDEO:
            return AppMediaSource_InitVideoTransceiver( pxMediaCtx, pxTransceiver );

        case TRANSCEIVER_TRACK_KIND_AUDIO:
            return AppMediaSource_InitAudioTransceiver( pxMediaCtx, pxTransceiver );

        default:
            LogWarn( "[KVSWebRTC] Unknown track kind: %d", ( int ) eTrackKind );
            return -2;
    }
}

/* ── Raw UART debug (bypasses FreeRTOS logging) ────────────────────────── */
/* Bounded spin with watchdog safety.  The original `while(!TDR-ready){}`
 * could hang forever if the UART became wedged (FIFO full, another task
 * holding the peripheral, etc.), which is exactly what was observed:
 * log trace ended mid-character on '[' and the board reset / disconnected.
 * At 600 MHz, 600000 iterations ≈ 1 ms — vastly more than one 115200-baud
 * byte (~87 µs).  If TDR-ready does not clear in that budget the UART is
 * wedged: pet the IWDG (so a run of dropped chars does not starve the
 * watchdog) and silently drop the character rather than hang the CPU.    */
extern void vPetWatchdog( void );
static inline void hook_raw_putc( char c )
{
    /* Hot-path raw UART trace: off by default (busy-wait UART writes
     * serialize into the frame/send loop). Build with -DKVS_RAW_TRACE=1
     * to re-enable for debugging. */
#if !defined( KVS_RAW_TRACE ) || ( KVS_RAW_TRACE == 0 )
    ( void ) c;
    return;
#endif
    for( uint32_t i = 0; i < 600000UL; i++ )
    {
        if( *(volatile uint32_t *)0x56000C1CUL & ( 1UL << 7 ) )
        {
            *(volatile uint32_t *)0x56000C28UL = ( uint32_t ) c;
            return;
        }
    }
    vPetWatchdog();
}
static void hook_raw_puts( const char *s ) { while( *s ) hook_raw_putc( *s++ ); }
static void hook_raw_dec( int v )
{
    char buf[ 12 ];
    int  n = 0;
    if( v < 0 ) { hook_raw_putc( '-' ); v = -v; }
    if( v == 0 ) { hook_raw_putc( '0' ); return; }
    while( v > 0 ) { buf[ n++ ] = '0' + ( v % 10 ); v /= 10; }
    while( n-- ) hook_raw_putc( buf[ n ] );
}

/* ── Media sink: forward encoded frames to all connected peers ──────────── */

static int32_t prvOnMediaSinkHook( void *         pvCustom,
                                   MediaFrame_t * pxFrame )
{
    AppContext_t *        pxCtx = ( AppContext_t * ) pvCustom;
    PeerConnectionFrame_t xPcFrame;
    int                   i;

    if( ( pvCustom == NULL ) || ( pxFrame == NULL ) )
        return -1;

    hook_raw_puts( "[H] enter sz=" );
    hook_raw_dec( ( int ) pxFrame->size );
    hook_raw_puts( " tk=" );
    hook_raw_dec( ( int ) pxFrame->trackKind );
    hook_raw_puts( " hwm=" );
    hook_raw_dec( ( int ) uxTaskGetStackHighWaterMark( NULL ) );
    hook_raw_puts( " heap=" );
    hook_raw_dec( ( int ) xPortGetFreeHeapSize() );
    hook_raw_puts( "\r\n" );

    xPcFrame.version        = PEER_CONNECTION_FRAME_CURRENT_VERSION;
    xPcFrame.presentationUs = pxFrame->timestampUs;
    xPcFrame.pData          = pxFrame->pData;
    xPcFrame.dataLength     = pxFrame->size;

    for( i = 0; i < AWS_MAX_VIEWER_NUM; i++ )
    {
        AppSession_t *    pxSession;
        Transceiver_t *   pxTransceiver = NULL;

        hook_raw_puts( "[H] it" );
        hook_raw_dec( i );
        hook_raw_puts( " top\r\n" );

        pxSession = &pxCtx->appSessions[ i ];

        hook_raw_puts( "[H] it" );
        hook_raw_dec( i );
        hook_raw_puts( " ps=" );
        hook_raw_dec( ( int )( ( uintptr_t ) pxSession >> 16 ) );
        hook_raw_puts( "_" );
        hook_raw_dec( ( int )( ( uintptr_t ) pxSession & 0xFFFF ) );
        hook_raw_puts( "\r\n" );

        if( pxFrame->trackKind == TRANSCEIVER_TRACK_KIND_VIDEO )
            pxTransceiver = &pxSession->transceivers[ DEMO_TRANSCEIVER_MEDIA_INDEX_VIDEO ];
        else if( pxFrame->trackKind == TRANSCEIVER_TRACK_KIND_AUDIO )
            pxTransceiver = &pxSession->transceivers[ DEMO_TRANSCEIVER_MEDIA_INDEX_AUDIO ];
        else
            break;

        hook_raw_puts( "[H] s" );
        hook_raw_dec( i );
        hook_raw_puts( " st=" );
        hook_raw_dec( ( int ) pxSession->peerConnectionSession.state );
        hook_raw_puts( "\r\n" );

        if( pxSession->peerConnectionSession.state == PEER_CONNECTION_SESSION_STATE_CONNECTION_READY )
        {
            hook_raw_puts( "[H] s" );
            hook_raw_dec( i );
            hook_raw_puts( " WF>\r\n" );
            PeerConnectionResult_t xResult =
                PeerConnection_WriteFrame( &pxSession->peerConnectionSession,
                                          pxTransceiver,
                                          &xPcFrame );
            hook_raw_puts( "[H] s" );
            hook_raw_dec( i );
            hook_raw_puts( " WF<r=" );
            hook_raw_dec( ( int ) xResult );
            hook_raw_puts( "\r\n" );
            if( xResult != PEER_CONNECTION_RESULT_OK )
            {
                LogWarn( "[KVSWebRTC] WriteFrame failed for session %d: %d", i, ( int ) xResult );
            }
        }

        hook_raw_puts( "[H] it" );
        hook_raw_dec( i );
        hook_raw_puts( " end\r\n" );
    }

    hook_raw_puts( "[H] for-exit\r\n" );
    hook_raw_puts( "[H] exit\r\n" );
    return 0;
}

/* ── KVStore config loading ─────────────────────────────────────────────── */

static void prvFreeConfigBuffers( void )
{
    if( pcRegionBuf )       { vPortFree( pcRegionBuf );      pcRegionBuf      = NULL; }
    if( pcChannelBuf )      { vPortFree( pcChannelBuf );     pcChannelBuf     = NULL; }
    if( pcEndpointBuf )     { vPortFree( pcEndpointBuf );    pcEndpointBuf    = NULL; }
    if( pcThingNameBuf )    { vPortFree( pcThingNameBuf );   pcThingNameBuf   = NULL; }
    if( pcRoleAliasBuf )    { vPortFree( pcRoleAliasBuf );   pcRoleAliasBuf   = NULL; }
    if( pcThingCertBuf )    { vPortFree( pcThingCertBuf );   pcThingCertBuf   = NULL; }
    if( pcPrivateKeyBuf )   { vPortFree( pcPrivateKeyBuf );  pcPrivateKeyBuf  = NULL; }
    if( pcCertPemBuf )      { vPortFree( pcCertPemBuf );     pcCertPemBuf     = NULL; }
    if( pcKeyPemBuf )       { vPortFree( pcKeyPemBuf );      pcKeyPemBuf      = NULL; }
}

static int prvLoadConfigFromKVStore( void )
{
    size_t xLen;

    prvFreeConfigBuffers();

    pcRegionBuf = KVStore_getStringHeap( CS_KVS_AWS_REGION, &xLen );
    if( ( pcRegionBuf == NULL ) || ( xLen == 0 ) )
    {
        LogError( "[KVSWebRTC] kvs_aws_region not set. Use: conf set kvs_aws_region <region>" );
        return -1;
    }

    pcChannelBuf = KVStore_getStringHeap( CS_KVS_CHANNEL_NAME, &xLen );
    if( ( pcChannelBuf == NULL ) || ( xLen == 0 ) )
    {
        LogError( "[KVSWebRTC] kvs_channel_name not set. Use: conf set kvs_channel_name <name>" );
        return -1;
    }

    /* Credentials: role-alias path (empty strings are acceptable for the
     * access-key path — app_common.c will use awsCreds instead if
     * AWS_ACCESS_KEY_ID is defined; here we only support role-alias).     */
    pcEndpointBuf   = KVStore_getStringHeap( CS_KVS_CREDENTIALS_ENDPOINT, &xLen );
    pcThingNameBuf  = KVStore_getStringHeap( CS_KVS_IOT_THING_NAME,       &xLen );
    pcRoleAliasBuf  = KVStore_getStringHeap( CS_KVS_IOT_ROLE_ALIAS,       &xLen );
    pcThingCertBuf  = KVStore_getStringHeap( CS_KVS_IOT_THING_CERT,       &xLen );
    pcPrivateKeyBuf = KVStore_getStringHeap( CS_KVS_IOT_PRIVATE_KEY,      &xLen );

    if( ( pcRoleAliasBuf == NULL ) || ( strlen( pcRoleAliasBuf ) == 0 ) )
    {
        LogError( "[KVSWebRTC] kvs_role_alias not set. Use: conf set kvs_role_alias <alias>" );
        return -1;
    }

    /* Point the runtime config globals at the loaded buffers */
    pcKvsAwsRegion           = pcRegionBuf;
    pcKvsChannelName         = pcChannelBuf;
    pcKvsCredentialsEndpoint = pcEndpointBuf   ? pcEndpointBuf   : "";
    pcKvsIotThingName        = pcThingNameBuf  ? pcThingNameBuf  : "";
    pcKvsIotRoleAlias        = pcRoleAliasBuf  ? pcRoleAliasBuf  : "";
    pcKvsIotThingCert        = pcThingCertBuf  ? pcThingCertBuf  : "";
    pcKvsIotPrivateKey       = pcPrivateKeyBuf ? pcPrivateKeyBuf : "";

    LogInfo( "[KVSWebRTC] Config loaded: region=%s channel=%s thing=%s",
             pcKvsAwsRegion, pcKvsChannelName, pcKvsIotThingName );
    return 0;
}

/* ── Load device cert + private key from PKCS#11 as PEM strings ────────── */

#define KVS_PEM_CERT_MAX_SIZE    2048U   /* generous for EC cert PEM       */
#define KVS_PEM_KEY_MAX_SIZE     512U    /* EC P-256 private key PEM       */

static int prvLoadCertAndKeyPem( void )
{
    int              lRet = -1;
    PkiObject_t      xCertObj;
    PkiObject_t      xKeyObj;
    mbedtls_x509_crt xCert;
    mbedtls_pk_context xPk;
    mbedtls_entropy_context xEntropy;
    size_t           uxPemLen = 0;

    mbedtls_x509_crt_init( &xCert );
    mbedtls_pk_init( &xPk );
    mbedtls_entropy_init( &xEntropy );

    /* Free previous buffers if any */
    if( pcCertPemBuf ) { vPortFree( pcCertPemBuf ); pcCertPemBuf = NULL; }
    if( pcKeyPemBuf )  { vPortFree( pcKeyPemBuf );  pcKeyPemBuf  = NULL; }

    /* ── Certificate ─────────────────────────────────────────────────── */
    xCertObj = xPkiObjectFromLabel( TLS_CERT_LABEL );

    if( xPkiReadCertificate( &xCert, &xCertObj ) != PKI_SUCCESS )
    {
        LogError( "[KVSWebRTC] Failed to read device certificate from PKCS#11." );
        goto cleanup;
    }

    /* Convert DER → PEM using mbedtls_pem_write_buffer */
    {
        size_t uxOlen = 0;

        /* First call to get required size */
        int lErr = mbedtls_pem_write_buffer(
                        "-----BEGIN CERTIFICATE-----\n",
                        "-----END CERTIFICATE-----\n",
                        xCert.raw.p, xCert.raw.len,
                        NULL, 0, &uxOlen );

        if( lErr != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || uxOlen == 0 )
        {
            LogError( "[KVSWebRTC] PEM size query failed for cert: %d", lErr );
            goto cleanup;
        }

        pcCertPemBuf = ( char * ) pvPortMalloc( uxOlen );
        if( pcCertPemBuf == NULL )
        {
            LogError( "[KVSWebRTC] Failed to allocate %u bytes for cert PEM.", ( unsigned ) uxOlen );
            goto cleanup;
        }

        lErr = mbedtls_pem_write_buffer(
                    "-----BEGIN CERTIFICATE-----\n",
                    "-----END CERTIFICATE-----\n",
                    xCert.raw.p, xCert.raw.len,
                    ( unsigned char * ) pcCertPemBuf, uxOlen, &uxPemLen );

        if( lErr != 0 )
        {
            LogError( "[KVSWebRTC] PEM write failed for cert: %d", lErr );
            vPortFree( pcCertPemBuf );
            pcCertPemBuf = NULL;
            goto cleanup;
        }
    }

    /* ── Private key ─────────────────────────────────────────────────── */
    xKeyObj = xPkiObjectFromLabel( TLS_KEY_PRV_LABEL );

    if( xPkiReadPrivateKey( &xPk, &xKeyObj,
                            mbedtls_entropy_func, &xEntropy ) != PKI_SUCCESS )
    {
        LogError( "[KVSWebRTC] Failed to read private key from PKCS#11." );
        goto cleanup;
    }

    pcKeyPemBuf = ( char * ) pvPortMalloc( KVS_PEM_KEY_MAX_SIZE );
    if( pcKeyPemBuf == NULL )
    {
        LogError( "[KVSWebRTC] Failed to allocate key PEM buffer." );
        goto cleanup;
    }

    if( mbedtls_pk_write_key_pem( &xPk,
                                  ( unsigned char * ) pcKeyPemBuf,
                                  KVS_PEM_KEY_MAX_SIZE ) != 0 )
    {
        LogError( "[KVSWebRTC] Failed to convert private key to PEM." );
        vPortFree( pcKeyPemBuf );
        pcKeyPemBuf = NULL;
        goto cleanup;
    }

    /* Point runtime config globals at the loaded PEM buffers */
    pcKvsIotThingCert  = pcCertPemBuf;
    pcKvsIotPrivateKey = pcKeyPemBuf;

    LogInfo( "[KVSWebRTC] Device cert and private key loaded from PKCS#11." );
    lRet = 0;

cleanup:
    mbedtls_x509_crt_free( &xCert );
    mbedtls_pk_free( &xPk );
    mbedtls_entropy_free( &xEntropy );
    return lRet;
}

/* ── Task entry point ───────────────────────────────────────────────────── */

void vKvsWebRtcTask( void * pvParameters )
{
    ( void ) pvParameters;

    BackoffAlgorithmContext_t xBackoff;
    uint16_t                  usNextBackoffMs;
    BackoffAlgorithmStatus_t  xBackoffStatus;

    LogInfo( "[KVSWebRTC] Task started." );

    /* Wait for network to be ready before any network I/O */
    configASSERT( xSystemEvents != NULL );
    ( void ) xEventGroupWaitBits( xSystemEvents,
                                  EVT_MASK_NET_CONNECTED,
                                  pdFALSE,   /* don't clear */
                                  pdTRUE,
                                  portMAX_DELAY );

    LogInfo( "[KVSWebRTC] Network ready, waiting for IOTCONNECT identity..." );

    /* ── ST bug-report WAN reachability probe (disabled after v1.3.0 MW upgrade) ── */
    /* NetDiag_Run( netif_get_interface( NETIF_STA ) ); */


    /*
     * Wait up to 60 s for the IOTCONNECT task to complete its identity flow
     * and parse the "vs" block.  If the event arrives, the runtime config
     * globals (pcKvsAwsRegion etc.) are already populated — no KVStore
     * read needed.  If the timeout expires, fall back to KVStore.
     */
    {
        EventBits_t uxBits = xEventGroupWaitBits( xSystemEvents,
                                                  EVT_MASK_IOTC_KVS_CONFIG,
                                                  pdFALSE,
                                                  pdTRUE,
                                                  pdMS_TO_TICKS( 60000U ) );

        if( uxBits & EVT_MASK_IOTC_KVS_CONFIG )
        {
            LogInfo( "[KVSWebRTC] KVS config received from IOTCONNECT identity." );
        }
        else
        {
            LogInfo( "[KVSWebRTC] IOTCONNECT identity timeout — will try KVStore." );
        }
    }

    /*
     * Wait for MQTT to finish connecting before starting KVS TLS operations.
     * Both use large mbedtls contexts; running them concurrently can exhaust
     * the 300 KB FreeRTOS heap and trigger a malloc-failed reset.
     */
    {
        LogInfo( "[KVSWebRTC] Waiting for MQTT connection before starting KVS..." );

        EventBits_t uxBits = xEventGroupWaitBits( xSystemEvents,
                                                  EVT_MASK_MQTT_CONNECTED,
                                                  pdFALSE,
                                                  pdTRUE,
                                                  pdMS_TO_TICKS( 120000U ) );

        if( uxBits & EVT_MASK_MQTT_CONNECTED )
        {
            LogInfo( "[KVSWebRTC] MQTT connected — proceeding with KVS init." );
        }
        else
        {
            LogWarn( "[KVSWebRTC] MQTT connection timeout — proceeding anyway." );
        }

        vPetWatchdog();
    }

    BackoffAlgorithm_InitializeParams( &xBackoff,
                                       KVS_RETRY_BACKOFF_BASE_S,
                                       KVS_RETRY_MAX_BACKOFF_S,
                                       BACKOFF_ALGORITHM_RETRY_FOREVER );

    for( ; ; )
    {
        vPetWatchdog();

        /* ── Check if config is ready (from IOTCONNECT or KVStore) ───── */
        if( ( pcKvsAwsRegion == NULL )    || ( pcKvsAwsRegion[0] == '\0' ) ||
            ( pcKvsChannelName == NULL )  || ( pcKvsChannelName[0] == '\0' ) ||
            ( pcKvsIotRoleAlias == NULL ) || ( pcKvsIotRoleAlias[0] == '\0' ) )
        {
            /* Runtime globals not set by IOTCONNECT — try KVStore */
            if( prvLoadConfigFromKVStore() != 0 )
            {
                LogWarn( "[KVSWebRTC] Config incomplete, retrying in 30 s." );
                vTaskDelay( pdMS_TO_TICKS( 30000U ) );
                continue;
            }
        }
        else
        {
            LogInfo( "[KVSWebRTC] Using IOTCONNECT-provided config." );
        }

        /* ── Load device cert + key from PKCS#11 for TLS mutual auth ── */
        if( prvLoadCertAndKeyPem() != 0 )
        {
            LogError( "[KVSWebRTC] Cert/key PEM load failed, retrying in 30 s." );
            vTaskDelay( pdMS_TO_TICKS( 30000U ) );
            continue;
        }

        /* ── Initialise SDK app context ──────────────────────────────── */
        /* Use raw UART to report heap — LogInfo itself may trigger malloc fail */
        {
            char buf[80];
            const char *p;
            snprintf( buf, sizeof(buf), "\r\nHEAP free=%u min=%u\r\n",
                      ( unsigned ) xPortGetFreeHeapSize(),
                      ( unsigned ) xPortGetMinimumEverFreeHeapSize() );
            /* Use hook_raw_putc (bounded spin + watchdog-pet). */
            for( p = buf; *p; p++ ) {
                hook_raw_putc( *p );
            }
        }

        /* xAppContext lives in PSRAM (.psram_bss, NOLOAD): bring PSRAM up
         * before zeroing it.  Idempotent — media init calls it again. */
        extern void MediaPort_EnsurePsram( void );
        MediaPort_EnsurePsram();

        memset( &xAppContext,            0, sizeof( xAppContext ) );
        memset( &xAppMediaSourceContext, 0, sizeof( xAppMediaSourceContext ) );

        /* NPU object detection (no-op unless ENABLE_AI_DETECTION). */
        AiDetection_Init();

        vPetWatchdog();
        LogInfo( "[KVSWebRTC] Calling AppMediaSource_Init..." );

        int lRet = AppMediaSource_Init( &xAppMediaSourceContext,
                                        prvOnMediaSinkHook,
                                        &xAppContext );
        if( lRet != 0 )
        {
            LogError( "[KVSWebRTC] AppMediaSource_Init failed: %d", lRet );
            goto retry;
        }

        vPetWatchdog();
        LogInfo( "[KVSWebRTC] Heap after media init: %u, calling AppCommon_Init...",
                 ( unsigned ) xPortGetFreeHeapSize() );

        lRet = AppCommon_Init( &xAppContext,
                               prvInitTransceiver,
                               &xAppMediaSourceContext );
        if( lRet != 0 )
        {
            LogError( "[KVSWebRTC] AppCommon_Init failed: %d", lRet );
            goto retry;
        }

        LogInfo( "[KVSWebRTC] Heap after AppCommon_Init: %u",
                 ( unsigned ) xPortGetFreeHeapSize() );

        /* Configure as master */
        static const char pcMasterId[] = "STM32N6-Master";
        memcpy( xAppContext.signalingControllerClientId,
                pcMasterId,
                sizeof( pcMasterId ) );
        xAppContext.signalingControllerClientIdLength = sizeof( pcMasterId ) - 1U;
        xAppContext.signalingControllerRole           = SIGNALING_ROLE_MASTER;

        LogInfo( "[KVSWebRTC] Connecting to KVS signaling channel '%s' in region '%s'...",
                 pcKvsChannelName, pcKvsAwsRegion );

        vPetWatchdog();

        /* Blocking — returns only on error or explicit stop */
        lRet = AppCommon_StartSignalingController( &xAppContext );
        AppCommon_WaitSignalingControllerStop( &xAppContext );

        if( lRet != 0 )
            LogError( "[KVSWebRTC] Signaling controller exited with error: %d", lRet );
        else
            LogInfo( "[KVSWebRTC] Signaling controller stopped." );

retry:
        prvFreeConfigBuffers();

        /* Exponential backoff before retry */
        xBackoffStatus = BackoffAlgorithm_GetNextBackoff( &xBackoff,
                                                          ( uint32_t ) xTaskGetTickCount(),
                                                          &usNextBackoffMs );
        if( xBackoffStatus == BackoffAlgorithmSuccess )
        {
            uint32_t ulDelayMs = ( uint32_t ) usNextBackoffMs *
                                 KVS_RETRY_BACKOFF_MULTIPLIER;
            LogInfo( "[KVSWebRTC] Retrying in %lu ms...", ulDelayMs );

            /* Pet watchdog periodically during the sleep */
            TickType_t xWakeTime = xTaskGetTickCount();
            TickType_t xEndTime  = xWakeTime + pdMS_TO_TICKS( ulDelayMs );

            while( xTaskGetTickCount() < xEndTime )
            {
                vPetWatchdog();
                vTaskDelay( pdMS_TO_TICKS( 2000U ) );
            }
        }
    }
}
