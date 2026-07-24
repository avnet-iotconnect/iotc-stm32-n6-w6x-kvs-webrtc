#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO

#include "../../cli/logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "task.h"

#include "core_mqtt.h"
#include "core_mqtt_agent.h"

#include "../../../Core/Inc/main.h"
#include "../../include/mbedtls_transport.h"
#include "../../include/sys_evt.h"
#include "../../kvstore/kvstore.h"
#include "../../sys/interrupt_handlers.h"
#include "../mqtt/mqtt_agent_task.h"
#include "../mqtt/subscription_manager.h"
#include "../ai/ai_detection.h"
#include "iotconnect_runtime.h"
#include "vendor/iotcl.h"
#include "vendor/iotcl_c2d.h"
#include "vendor/iotcl_cfg.h"
#include "vendor/iotcl_dra_discovery.h"
#include "vendor/iotcl_dra_identity.h"
#include "vendor/iotcl_dra_url.h"
#include "vendor/cJSON.h"

#include "../kvs_webrtc/port/kvs_runtime_config.h"

#define IOTCONNECT_HTTP_PORT                     ( 443U )
#define IOTCONNECT_HTTP_RECV_TIMEOUT_MS          ( 2000U )
#define IOTCONNECT_HTTP_SEND_TIMEOUT_MS          ( 2000U )
#define IOTCONNECT_HTTP_INITIAL_BUFFER_SIZE      ( 2048U )
#define IOTCONNECT_HTTP_MAX_BUFFER_SIZE          ( 16384U )
#define IOTCONNECT_HTTP_TIMEOUT_RETRIES          ( 3U )
#define IOTCONNECT_EVENT_QUEUE_LENGTH            ( 8U )
#define IOTCONNECT_DEMO_POLL_INTERVAL_MS         ( 200U )
/* Heartbeat publish so live values (LED/button state + NPU ai_people/
 * ai_top_conf/ai_infer_ms) reach IOTCONNECT continuously — without this the
 * demo task only published on a C2D command or a button press, so the boot
 * telemetry (ai_people=0, before the camera ran) was the only sample seen. */
#define IOTCONNECT_DEMO_PUBLISH_PERIOD_MS        ( 5000U )
#define IOTCONNECT_SAMPLE_PERIOD_MS              ( 5000U )
#define IOTCONNECT_ACK_MESSAGE_MAX_LEN           ( 96U )
#define IOTCONNECT_TOPIC_BUFFER_LEN              ( 256U )
#define IOTCONNECT_DEMO_BUTTON_EVENT             ( 1U << 0 )
#define IOTCONNECT_LED_RED_MASK                  ( 1U << 0 )
#define IOTCONNECT_LED_GREEN_MASK                ( 1U << 1 )
#define IOTCONNECT_APP_VERSION                   "00.01.00"

typedef enum IoTConnectCloud
{
    IOTCONNECT_CLOUD_AWS = 0,
    IOTCONNECT_CLOUD_AZURE
} IoTConnectCloud_t;

typedef enum IoTConnectQueuedEventType
{
    IOTCONNECT_EVENT_CMD_ACK = 0,
    IOTCONNECT_EVENT_OTA_ACK,
    IOTCONNECT_EVENT_DEMO_LED_UPDATE
} IoTConnectQueuedEventType_t;

typedef struct IoTConnectQueuedEvent
{
    IoTConnectQueuedEventType_t xType;
    int lStatus;
    char pcAckId[ IOTCL_MAX_ACK_LENGTH + 1 ];
    char pcMessage[ IOTCONNECT_ACK_MESSAGE_MAX_LEN ];
    uint8_t ucLedMask;
    uint8_t ucLedValueMask;
} IoTConnectQueuedEvent_t;

typedef struct IoTConnectRuntimeContext
{
    BaseType_t xReady;
    IoTConnectCloud_t xCloud;
    IoTConnectAppMode_t xAppMode;
    BaseType_t xLedRedOn;
    BaseType_t xLedGreenOn;
    BaseType_t xButtonPressed;
    QueueHandle_t xEventQueue;
    EventGroupHandle_t xButtonEvents;
} IoTConnectRuntimeContext_t;

struct MQTTAgentCommandContext
{
    TaskHandle_t xTaskToNotify;
};

static IoTConnectRuntimeContext_t xIoTConnectRuntime = { 0 };

static void prvCopyString( char * pcDest,
                           size_t uxDestLen,
                           const char * pcSrc );
static char * prvDupKvString( KVStoreKey_t xKey );
static IoTConnectCloud_t prvGetCloudType( const char * pcCloudName );
static IoTConnectAppMode_t prvGetAppModeFromString( const char * pcModeName );
static BaseType_t prvReadConfigStrings( char ** ppcCpid,
                                        char ** ppcEnv,
                                        char ** ppcDuid );
static void prvResetRuntimeState( void );
static BaseType_t prvIsStringBlank( const char * pcValue );
static BaseType_t prvStringEqualsIgnoreCase( const char * pcLeft,
                                             const char * pcRight );
static char * prvFindHeaderValue( char * pcHeaders,
                                  const char * pcHeaderName );
static BaseType_t prvHttpGetJson( const IotclDraUrlContext * pxUrlCtx,
                                  const char * pcRootCaLabel,
                                  char ** ppcBody );
static BaseType_t prvRunIdentityFlow( const char * pcCpid,
                                      const char * pcEnv,
                                      const char * pcDuid,
                                      char ** ppcIdentityJson );
static BaseType_t prvBootstrapIoTConnect( void );
static void prvApplyAwsIdentityWorkaround( void );
static BaseType_t prvTryLoadCachedIdentity( void );
static BaseType_t prvSaveCachedIdentity( const char * pcIdentityJson );
static void prvMqttPublishCommandCallback( MQTTAgentCommandContext_t * pxCommandContext,
                                           MQTTAgentReturnInfo_t * pxReturnInfo );
static void prvIoTConnectMqttSendCallback( const char * pcTopic,
                                           const char * pcJsonStr );
static BaseType_t prvSubscribeToC2DTopic( MQTTAgentHandle_t xMqttHandle );
static void prvC2DIncomingPublishCallback( void * pvIncomingPublishCallbackContext,
                                           MQTTPublishInfo_t * pxPublishInfo );
static void prvDemoButtonEdgeCallback( void * pvContext );
static BaseType_t prvReadUserButtonPressed( void );
static void prvRefreshLedStateFromHardware( void );
static void prvSetLedState( uint8_t ucLedMask,
                            uint8_t ucLedValueMask );
static BaseType_t prvPublishDemoTelemetry( void );
static BaseType_t prvPublishSampleTelemetry( void );
static void prvQueueEvent( const IoTConnectQueuedEvent_t * pxEvent );
static void prvQueueCmdAckEvent( const char * pcAckId,
                                 int lStatus,
                                 const char * pcMessage );
static void prvQueueOtaAckEvent( const char * pcAckId,
                                 int lStatus,
                                 const char * pcMessage );
static BaseType_t prvParseDemoLedCommand( const char * pcCommand,
                                          uint8_t * pucLedMask,
                                          uint8_t * pucLedValueMask );
static void prvDemoCommandCallback( IotclC2dEventData xEventData );
static void prvDemoOtaCallback( IotclC2dEventData xEventData );
static void prvSampleCommandCallback( IotclC2dEventData xEventData );
static void prvSampleOtaCallback( IotclC2dEventData xEventData );
static BaseType_t prvHandleQueuedCommandEvent( const IoTConnectQueuedEvent_t * pxEvent,
                                               BaseType_t * pxPublishTelemetry );
static BaseType_t prvHandleQueuedAckEvent( const IoTConnectQueuedEvent_t * pxEvent );
static void vIoTConnectDemoTask( void * pvParameters );
static void vIoTConnectSampleTask( void * pvParameters );
static void prvParseIdentityKvsConfig( const char * pcIdentityJson );

static void prvCopyString( char * pcDest,
                           size_t uxDestLen,
                           const char * pcSrc )
{
    if( uxDestLen == 0U )
    {
        return;
    }

    if( pcSrc == NULL )
    {
        pcDest[ 0 ] = '\0';
        return;
    }

    ( void ) strncpy( pcDest, pcSrc, uxDestLen - 1U );
    pcDest[ uxDestLen - 1U ] = '\0';
}

static BaseType_t prvIsStringBlank( const char * pcValue )
{
    return ( BaseType_t ) ( ( pcValue == NULL ) || ( pcValue[ 0 ] == '\0' ) );
}

static char * prvDupKvString( KVStoreKey_t xKey )
{
    size_t uxSize = KVStore_getSize( xKey );
    char * pcValue = NULL;

    if( uxSize == 0U )
    {
        return NULL;
    }

    pcValue = pvPortMalloc( uxSize );

    if( pcValue == NULL )
    {
        LogError( "Failed to allocate %lu bytes for KV string.", ( unsigned long ) uxSize );
    }
    else
    {
        ( void ) KVStore_getString( xKey, pcValue, uxSize );
        pcValue[ uxSize - 1U ] = '\0';
    }

    return pcValue;
}

static BaseType_t prvStringEqualsIgnoreCase( const char * pcLeft,
                                             const char * pcRight )
{
    int lResult = 0;

    if( ( pcLeft == NULL ) || ( pcRight == NULL ) )
    {
        return pdFALSE;
    }

    while( ( *pcLeft != '\0' ) && ( *pcRight != '\0' ) )
    {
        lResult = tolower( ( unsigned char ) *pcLeft ) - tolower( ( unsigned char ) *pcRight );

        if( lResult != 0 )
        {
            return pdFALSE;
        }

        pcLeft++;
        pcRight++;
    }

    return ( BaseType_t ) ( ( *pcLeft == '\0' ) && ( *pcRight == '\0' ) );
}

AppBrokerType_t xAppGetBrokerType( void )
{
    AppBrokerType_t xBrokerType = APP_BROKER_AWS;
    char * pcBrokerType = prvDupKvString( CS_CORE_BROKER_TYPE );

    if( pcBrokerType != NULL )
    {
        if( prvStringEqualsIgnoreCase( pcBrokerType, "mosquitto" ) )
        {
            xBrokerType = APP_BROKER_MOSQUITTO;
        }
        else if( prvStringEqualsIgnoreCase( pcBrokerType, "iotconnect" ) )
        {
            xBrokerType = APP_BROKER_IOTCONNECT;
        }

        vPortFree( pcBrokerType );
    }

    return xBrokerType;
}

BaseType_t xAppIsIoTConnectBroker( void )
{
    return ( BaseType_t ) ( xAppGetBrokerType() == APP_BROKER_IOTCONNECT );
}

BaseType_t xIoTConnectRuntimeIsReady( void )
{
    return xIoTConnectRuntime.xReady;
}

IoTConnectAppMode_t xIoTConnectGetAppMode( void )
{
    return xIoTConnectRuntime.xAppMode;
}

const char * pcIoTConnectMqttEndpoint( void )
{
    IotclMqttConfig * pxMqttConfig = iotcl_mqtt_get_config();

    if( ( xIoTConnectRuntime.xReady == pdFALSE ) || ( pxMqttConfig == NULL ) )
    {
        return NULL;
    }

    return pxMqttConfig->host;
}

const char * pcIoTConnectMqttClientId( void )
{
    IotclMqttConfig * pxMqttConfig = iotcl_mqtt_get_config();

    if( ( xIoTConnectRuntime.xReady == pdFALSE ) || ( pxMqttConfig == NULL ) )
    {
        return NULL;
    }

    return pxMqttConfig->client_id;
}

const char * pcIoTConnectMqttUserName( void )
{
    IotclMqttConfig * pxMqttConfig = iotcl_mqtt_get_config();

    if( ( xIoTConnectRuntime.xReady == pdFALSE ) || ( pxMqttConfig == NULL ) )
    {
        return NULL;
    }

    return pxMqttConfig->username;
}

uint16_t usIoTConnectMqttPort( void )
{
    return ( uint16_t ) IOTCL_MQTT_PORT;
}

static void prvResetRuntimeState( void )
{
    xIoTConnectRuntime.xReady = pdFALSE;
}

static IoTConnectCloud_t prvGetCloudType( const char * pcCloudName )
{
    if( prvStringEqualsIgnoreCase( pcCloudName, "azure" ) )
    {
        return IOTCONNECT_CLOUD_AZURE;
    }

    return IOTCONNECT_CLOUD_AWS;
}

static IoTConnectAppMode_t prvGetAppModeFromString( const char * pcModeName )
{
    if( prvStringEqualsIgnoreCase( pcModeName, "sample" ) )
    {
        return IOTCONNECT_APP_MODE_SAMPLE;
    }

    return IOTCONNECT_APP_MODE_DEMO;
}

static BaseType_t prvReadConfigStrings( char ** ppcCpid,
                                        char ** ppcEnv,
                                        char ** ppcDuid )
{
    BaseType_t xStatus = pdFALSE;
    char * pcCloud = prvDupKvString( CS_IOTCONNECT_CLOUD );
    char * pcCpid = prvDupKvString( CS_IOTCONNECT_CPID );
    char * pcEnv = prvDupKvString( CS_IOTCONNECT_ENV );
    char * pcThingName = NULL;
    char * pcMode = prvDupKvString( CS_IOTCONNECT_APP_MODE );

    if( ( pcCloud == NULL ) || ( pcCpid == NULL ) || ( pcEnv == NULL ) || ( pcMode == NULL ) )
    {
        LogError( "IOTCONNECT configuration is incomplete in KVStore." );
        goto cleanup;
    }

    xIoTConnectRuntime.xCloud = prvGetCloudType( pcCloud );
    xIoTConnectRuntime.xAppMode = prvGetAppModeFromString( pcMode );

    if( prvIsStringBlank( pcCpid ) || prvIsStringBlank( pcEnv ) )
    {
        LogError( "IOTCONNECT CPID and ENV must be configured." );
        goto cleanup;
    }

    pcThingName = prvDupKvString( CS_CORE_THING_NAME );

    if( ( pcThingName == NULL ) || prvIsStringBlank( pcThingName ) )
    {
        LogError( "IOTCONNECT requires thing_name, but thing_name is empty." );
        goto cleanup;
    }

    *ppcCpid = pcCpid;
    *ppcEnv = pcEnv;
    *ppcDuid = pcThingName;
    pcThingName = NULL;
    xStatus = pdTRUE;

cleanup:
    if( xStatus == pdFALSE )
    {
        if( pcCpid != NULL )
        {
            vPortFree( pcCpid );
        }

        if( pcEnv != NULL )
        {
            vPortFree( pcEnv );
        }

    }

    if( pcCloud != NULL )
    {
        vPortFree( pcCloud );
    }

    if( pcThingName != NULL )
    {
        vPortFree( pcThingName );
    }

    if( pcMode != NULL )
    {
        vPortFree( pcMode );
    }

    return xStatus;
}

static char * prvFindHeaderValue( char * pcHeaders,
                                  const char * pcHeaderName )
{
    size_t uxNameLen = strlen( pcHeaderName );
    char * pcCursor = pcHeaders;

    while( ( pcCursor != NULL ) && ( *pcCursor != '\0' ) )
    {
        char * pcLineEnd = strstr( pcCursor, "\r\n" );

        if( pcLineEnd == NULL )
        {
            pcLineEnd = pcCursor + strlen( pcCursor );
        }

        if( ( ( size_t ) ( pcLineEnd - pcCursor ) ) > uxNameLen )
        {
            if( strncmp( pcCursor, pcHeaderName, uxNameLen ) == 0 )
            {
                pcCursor += uxNameLen;

                while( *pcCursor == ' ' )
                {
                    pcCursor++;
                }

                return pcCursor;
            }
        }

        if( *pcLineEnd == '\0' )
        {
            break;
        }

        pcCursor = pcLineEnd + 2;
    }

    return NULL;
}

static BaseType_t prvHttpGetJson( const IotclDraUrlContext * pxUrlCtx,
                                  const char * pcRootCaLabel,
                                  char ** ppcBody )
{
    BaseType_t xStatus = pdFALSE;
    NetworkContext_t * pxNetworkContext = NULL;
    char * pcRequest = NULL;
    char * pcResponse = NULL;
    char * pcBody = NULL;
    size_t uxResponseLen = 0U;
    size_t uxResponseCap = IOTCONNECT_HTTP_INITIAL_BUFFER_SIZE;
    size_t uxBodyLen = 0U;
    size_t uxHeaderLen = 0U;
    size_t uxExpectedBodyLen = SIZE_MAX;
    uint32_t ulTimeoutCount = 0U;
    char * pcHeaderEnd = NULL;
    const char * pcHost = NULL;
    const char * pcResource = NULL;
    int lHttpStatus = 0;
    PkiObject_t pxRootCaChain[ 1 ];

    *ppcBody = NULL;

    if( ( pxUrlCtx == NULL ) || ( pcRootCaLabel == NULL ) )
    {
        LogError( "Invalid IOTCONNECT HTTPS request arguments." );
        return pdFALSE;
    }

    pcHost = iotcl_dra_url_get_hostname( pxUrlCtx );
    pcResource = iotcl_dra_url_get_resource( pxUrlCtx );
    pxRootCaChain[ 0 ] = xPkiObjectFromLabel( pcRootCaLabel );

    if( ( pcHost == NULL ) || ( pcResource == NULL ) || ( iotcl_dra_url_is_https( pxUrlCtx ) == false ) )
    {
        LogError( "IOTCONNECT HTTPS helper received an invalid URL." );
        return pdFALSE;
    }

    pxNetworkContext = mbedtls_transport_allocate();

    if( pxNetworkContext == NULL )
    {
        goto cleanup;
    }

    if( mbedtls_transport_configure( pxNetworkContext,
                                     NULL,
                                     NULL,
                                     NULL,
                                     pxRootCaChain,
                                     1U ) != TLS_TRANSPORT_SUCCESS )
    {
        LogError( "Failed to configure HTTPS transport for IOTCONNECT REST call." );
        goto cleanup;
    }

    if( mbedtls_transport_connect( pxNetworkContext,
                                   pcHost,
                                   IOTCONNECT_HTTP_PORT,
                                   IOTCONNECT_HTTP_RECV_TIMEOUT_MS,
                                   IOTCONNECT_HTTP_SEND_TIMEOUT_MS ) != TLS_TRANSPORT_SUCCESS )
    {
        LogError( "Failed to connect to IOTCONNECT REST endpoint %s.", pcHost );
        goto cleanup;
    }

    {
        int lReqLen = snprintf( NULL,
                                0,
                                "GET %s HTTP/1.0\r\n"
                                "Host: %s\r\n"
                                "User-Agent: stm32n6-iotconnect/1.0\r\n"
                                "Accept: application/json\r\n"
                                "Accept-Encoding: identity\r\n"
                                "Connection: close\r\n\r\n",
                                pcResource,
                                pcHost );

        pcRequest = pvPortMalloc( ( size_t ) lReqLen + 1U );

        if( pcRequest == NULL )
        {
            goto cleanup;
        }

        ( void ) snprintf( pcRequest,
                           ( size_t ) lReqLen + 1U,
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: stm32n6-iotconnect/1.0\r\n"
                           "Accept: application/json\r\n"
                           "Accept-Encoding: identity\r\n"
                           "Connection: close\r\n\r\n",
                           pcResource,
                           pcHost );
    }

    if( mbedtls_transport_send( pxNetworkContext,
                                pcRequest,
                                strlen( pcRequest ) ) <= 0 )
    {
        LogError( "Failed to send IOTCONNECT HTTPS request for %s.", pcResource );
        goto cleanup;
    }

    pcResponse = pvPortMalloc( uxResponseCap + 1U );

    if( pcResponse == NULL )
    {
        goto cleanup;
    }

    while( 1 )
    {
        int32_t lBytesRead = 0;

        if( uxResponseLen == uxResponseCap )
        {
            size_t uxNewCap = uxResponseCap * 2U;
            char * pcNewResponse = NULL;

            if( uxNewCap > IOTCONNECT_HTTP_MAX_BUFFER_SIZE )
            {
                LogError( "IOTCONNECT HTTP response exceeded %lu bytes.", ( unsigned long ) IOTCONNECT_HTTP_MAX_BUFFER_SIZE );
                goto cleanup;
            }

            pcNewResponse = pvPortMalloc( uxNewCap + 1U );

            if( pcNewResponse == NULL )
            {
                goto cleanup;
            }

            memcpy( pcNewResponse, pcResponse, uxResponseLen );
            vPortFree( pcResponse );
            pcResponse = pcNewResponse;
            uxResponseCap = uxNewCap;
        }

        lBytesRead = mbedtls_transport_recv( pxNetworkContext,
                                             &( pcResponse[ uxResponseLen ] ),
                                             uxResponseCap - uxResponseLen );

        if( lBytesRead > 0 )
        {
            uxResponseLen += ( size_t ) lBytesRead;
            pcResponse[ uxResponseLen ] = '\0';
            ulTimeoutCount = 0U;

            if( pcHeaderEnd == NULL )
            {
                pcHeaderEnd = strstr( pcResponse, "\r\n\r\n" );

                if( pcHeaderEnd != NULL )
                {
                    char * pcContentLength = NULL;

                    uxHeaderLen = ( size_t ) ( ( pcHeaderEnd + 4 ) - pcResponse );
                    *pcHeaderEnd = '\0';

                    if( sscanf( pcResponse, "HTTP/%*d.%*d %d", &lHttpStatus ) != 1 )
                    {
                        LogError( "Failed to parse IOTCONNECT HTTP status line." );
                        goto cleanup;
                    }

                    if( lHttpStatus != 200 )
                    {
                        LogError( "IOTCONNECT HTTP GET failed with status %d.", lHttpStatus );
                        goto cleanup;
                    }

                    pcContentLength = prvFindHeaderValue( pcResponse, "Content-Length:" );

                    if( pcContentLength != NULL )
                    {
                        uxExpectedBodyLen = strtoul( pcContentLength, NULL, 10 );
                    }

                    *pcHeaderEnd = '\r';
                }
            }

            if( ( pcHeaderEnd != NULL ) &&
                ( uxExpectedBodyLen != SIZE_MAX ) &&
                ( uxResponseLen >= ( uxHeaderLen + uxExpectedBodyLen ) ) )
            {
                break;
            }
        }
        else if( lBytesRead == 0 )
        {
            ulTimeoutCount++;

            if( ( pcHeaderEnd != NULL ) &&
                ( uxExpectedBodyLen != SIZE_MAX ) &&
                ( uxResponseLen >= ( uxHeaderLen + uxExpectedBodyLen ) ) )
            {
                break;
            }

            if( ulTimeoutCount >= IOTCONNECT_HTTP_TIMEOUT_RETRIES )
            {
                break;
            }
        }
        else
        {
            break;
        }
    }

    if( pcHeaderEnd == NULL )
    {
        LogError( "IOTCONNECT HTTP response did not contain headers." );
        goto cleanup;
    }

    if( uxExpectedBodyLen == SIZE_MAX )
    {
        uxExpectedBodyLen = uxResponseLen - uxHeaderLen;
    }

    if( uxResponseLen < ( uxHeaderLen + uxExpectedBodyLen ) )
    {
        LogError( "IOTCONNECT HTTP response was truncated." );
        goto cleanup;
    }

    uxBodyLen = uxExpectedBodyLen;
    pcBody = pvPortMalloc( uxBodyLen + 1U );

    if( pcBody == NULL )
    {
        goto cleanup;
    }

    memcpy( pcBody, &( pcResponse[ uxHeaderLen ] ), uxBodyLen );
    pcBody[ uxBodyLen ] = '\0';
    *ppcBody = pcBody;
    pcBody = NULL;
    xStatus = pdTRUE;

cleanup:
    if( pxNetworkContext != NULL )
    {
        mbedtls_transport_disconnect( pxNetworkContext );
        mbedtls_transport_free( pxNetworkContext );
    }

    if( pcRequest != NULL )
    {
        vPortFree( pcRequest );
    }

    if( pcResponse != NULL )
    {
        vPortFree( pcResponse );
    }

    if( pcBody != NULL )
    {
        vPortFree( pcBody );
    }

    return xStatus;
}

static void prvApplyAwsIdentityWorkaround( void )
{
    IotclMqttConfig * pxMqttConfig = iotcl_mqtt_get_config();

    if( ( xIoTConnectRuntime.xCloud == IOTCONNECT_CLOUD_AWS ) &&
        ( pxMqttConfig != NULL ) &&
        ( pxMqttConfig->username != NULL ) )
    {
        iotcl_free( pxMqttConfig->username );
        pxMqttConfig->username = NULL;
    }
}

static void prvClearCachedIdentity( void )
{
    BaseType_t xCleared = pdFALSE;

    if( ( KVStore_setUInt32( CS_IOTCONNECT_CACHE_VALID, 0U ) == pdTRUE ) &&
        ( KVStore_setString( CS_IOTCONNECT_IDENTITY_JSON, "" ) == pdTRUE ) )
    {
        xCleared = KVStore_xCommitChanges();
    }

    if( xCleared == pdFALSE )
    {
        LogWarn( "Failed to clear IOTCONNECT identity cache." );
    }
}

static BaseType_t prvTryLoadCachedIdentity( void )
{
    BaseType_t xSuccess = pdFALSE;
    uint32_t ulCacheValid = KVStore_getUInt32( CS_IOTCONNECT_CACHE_VALID, &xSuccess );

    if( ( xSuccess == pdFALSE ) || ( ulCacheValid == 0U ) )
    {
        return pdFALSE;
    }

    /*
     * The raw IOTCONNECT identity JSON can exceed the platform KV max value
     * length (256 bytes). Legacy firmware persisted that full blob and later
     * hit a read-time assert in the littlefs backend. Treat any stored cache as
     * unsupported, clear it without reading the blob back, and refresh from the
     * REST API instead.
     */
    LogWarn( "Ignoring legacy IOTCONNECT identity cache. Refreshing it from REST API." );
    prvClearCachedIdentity();
    return pdFALSE;
}

static BaseType_t prvSaveCachedIdentity( const char * pcIdentityJson )
{
    ( void ) pcIdentityJson;

    /*
     * Do not persist the full IOTCONNECT identity payload in KV storage.
     * The response is larger than the platform KV value limit, and caching it
     * can corrupt the stored entry and assert on the next boot.
     */
    return pdTRUE;
}

/* ── KVS WebRTC config from IoTConnect identity "vs" block ─────────────── */

/* Heap buffers for parsed vs fields — persist for the lifetime of the app */
static char * pcVsRegion      = NULL;
static char * pcVsChannelName = NULL;
static char * pcVsEndpoint    = NULL;
static char * pcVsRoleAlias   = NULL;
static char * pcVsThingName   = NULL;

/*
 * Parse the IoTConnect identity response JSON for the "vs" (video streaming)
 * block.  If present, extract:
 *   - AWS region and channel name from vs.carn (KVS signaling channel ARN)
 *   - Credentials endpoint and role alias from vs.url
 * Populate the KVS runtime config globals and signal EVT_MASK_IOTC_KVS_CONFIG.
 *
 * ARN format : arn:aws:kinesisvideo:<region>:<account>:channel/<name>/<ts>
 * URL format : https://<endpoint>/role-aliases/<alias>/credentials
 */
static void prvParseIdentityKvsConfig( const char * pcIdentityJson )
{
    cJSON * pxRoot = NULL;
    cJSON * pxD    = NULL;
    cJSON * pxVs   = NULL;
    const char * pcCarn = NULL;
    const char * pcUrl  = NULL;

    if( pcIdentityJson == NULL )
    {
        return;
    }

    pxRoot = cJSON_Parse( pcIdentityJson );

    if( pxRoot == NULL )
    {
        return;
    }

    pxD = cJSON_GetObjectItem( pxRoot, "d" );

    if( ( pxD == NULL ) || !cJSON_IsObject( pxD ) )
    {
        goto done;
    }

    /* The "vs" block lives at d.p.vs in the IoTConnect identity response */
    {
        cJSON * pxP = cJSON_GetObjectItem( pxD, "p" );

        if( ( pxP != NULL ) && cJSON_IsObject( pxP ) )
        {
            pxVs = cJSON_GetObjectItem( pxP, "vs" );
        }
    }

    /* Fallback: also check d.vs in case future API versions move it */
    if( ( pxVs == NULL ) || !cJSON_IsObject( pxVs ) )
    {
        pxVs = cJSON_GetObjectItem( pxD, "vs" );
    }

    if( ( pxVs == NULL ) || !cJSON_IsObject( pxVs ) )
    {
        LogInfo( "IoTConnect identity has no 'vs' block (checked d.p.vs and d.vs) — "
                 "KVS WebRTC not enabled for this device." );
        goto done;
    }

    /* ── Extract carn ──────────────────────────────────────────────────── */
    {
        cJSON * pxCarn = cJSON_GetObjectItem( pxVs, "carn" );

        if( ( pxCarn == NULL ) || !cJSON_IsString( pxCarn ) ||
            ( cJSON_GetStringValue( pxCarn ) == NULL ) )
        {
            LogWarn( "vs.carn missing or invalid in identity response." );
            goto done;
        }

        pcCarn = cJSON_GetStringValue( pxCarn );
    }

    /* ── Extract url ───────────────────────────────────────────────────── */
    {
        cJSON * pxUrl = cJSON_GetObjectItem( pxVs, "url" );

        if( ( pxUrl != NULL ) && cJSON_IsString( pxUrl ) )
        {
            pcUrl = cJSON_GetStringValue( pxUrl );
        }
    }

    /* ── Parse region + channel name from ARN ──────────────────────────── */
    /* arn:aws:kinesisvideo:<region>:<account>:channel/<name>/<ts>          */
    {
        const char * p = pcCarn;
        int iColon = 0;
        const char * pcRegionStart = NULL;
        const char * pcRegionEnd   = NULL;

        /* Walk to the 4th colon-delimited field (region) */
        while( *p != '\0' )
        {
            if( *p == ':' )
            {
                iColon++;

                if( iColon == 3 )
                {
                    pcRegionStart = p + 1;
                }
                else if( iColon == 4 )
                {
                    pcRegionEnd = p;
                    break;
                }
            }

            p++;
        }

        if( ( pcRegionStart == NULL ) || ( pcRegionEnd == NULL ) ||
            ( pcRegionEnd <= pcRegionStart ) )
        {
            LogWarn( "Could not parse region from vs.carn: %s", pcCarn );
            goto done;
        }

        size_t uxRegionLen = ( size_t )( pcRegionEnd - pcRegionStart );
        pcVsRegion = pvPortMalloc( uxRegionLen + 1U );

        if( pcVsRegion == NULL )
        {
            goto done;
        }

        memcpy( pcVsRegion, pcRegionStart, uxRegionLen );
        pcVsRegion[ uxRegionLen ] = '\0';

        /* Channel name: after "channel/" up to the next "/" */
        const char * pcChanPrefix = strstr( pcCarn, "channel/" );

        if( pcChanPrefix != NULL )
        {
            pcChanPrefix += 8; /* skip "channel/" */
            const char * pcChanEnd = strchr( pcChanPrefix, '/' );

            if( pcChanEnd == NULL )
            {
                pcChanEnd = pcChanPrefix + strlen( pcChanPrefix );
            }

            size_t uxChanLen = ( size_t )( pcChanEnd - pcChanPrefix );
            pcVsChannelName = pvPortMalloc( uxChanLen + 1U );

            if( pcVsChannelName != NULL )
            {
                memcpy( pcVsChannelName, pcChanPrefix, uxChanLen );
                pcVsChannelName[ uxChanLen ] = '\0';
            }
        }
    }

    /* ── Parse credentials endpoint + role alias from URL ──────────────── */
    /* https://<endpoint>/role-aliases/<alias>/credentials                  */
    if( pcUrl != NULL )
    {
        const char * pcHostStart = pcUrl;

        /* Skip "https://" */
        if( strncmp( pcHostStart, "https://", 8 ) == 0 )
        {
            pcHostStart += 8;
        }

        /* Endpoint = hostname (up to first '/') */
        const char * pcHostEnd = strchr( pcHostStart, '/' );

        if( pcHostEnd != NULL )
        {
            size_t uxHostLen = ( size_t )( pcHostEnd - pcHostStart );
            pcVsEndpoint = pvPortMalloc( uxHostLen + 1U );

            if( pcVsEndpoint != NULL )
            {
                memcpy( pcVsEndpoint, pcHostStart, uxHostLen );
                pcVsEndpoint[ uxHostLen ] = '\0';
            }
        }

        /* Role alias: between "/role-aliases/" and "/credentials" */
        const char * pcRaPrefix = strstr( pcUrl, "/role-aliases/" );

        if( pcRaPrefix != NULL )
        {
            pcRaPrefix += 14; /* skip "/role-aliases/" */
            const char * pcRaEnd = strstr( pcRaPrefix, "/credentials" );

            if( pcRaEnd == NULL )
            {
                pcRaEnd = pcRaPrefix + strlen( pcRaPrefix );
            }

            size_t uxRaLen = ( size_t )( pcRaEnd - pcRaPrefix );
            pcVsRoleAlias = pvPortMalloc( uxRaLen + 1U );

            if( pcVsRoleAlias != NULL )
            {
                memcpy( pcVsRoleAlias, pcRaPrefix, uxRaLen );
                pcVsRoleAlias[ uxRaLen ] = '\0';
            }
        }
    }

    /* ── Thing name = MQTT client_id from the identity response ────────── */
    {
        IotclMqttConfig * pxMqtt = iotcl_mqtt_get_config();

        if( ( pxMqtt != NULL ) && ( pxMqtt->client_id != NULL ) )
        {
            size_t uxLen = strlen( pxMqtt->client_id );
            pcVsThingName = pvPortMalloc( uxLen + 1U );

            if( pcVsThingName != NULL )
            {
                memcpy( pcVsThingName, pxMqtt->client_id, uxLen + 1U );
            }
        }
    }

    /* ── Populate KVS runtime config globals ───────────────────────────── */
    if( ( pcVsRegion != NULL ) && ( pcVsChannelName != NULL ) )
    {
        pcKvsAwsRegion           = pcVsRegion;
        pcKvsChannelName         = pcVsChannelName;
        pcKvsCredentialsEndpoint = ( pcVsEndpoint  != NULL ) ? pcVsEndpoint  : "";
        pcKvsIotRoleAlias        = ( pcVsRoleAlias != NULL ) ? pcVsRoleAlias : "";
        pcKvsIotThingName        = ( pcVsThingName != NULL ) ? pcVsThingName : "";

        /* Cert/key PEM strings remain empty here — the KVS networking layer
         * must load them from the PKCS#11 store (tls_cert / tls_key_priv)
         * at TLS connection time, same as the IoTConnect MQTT transport.  */

        LogInfo( "KVS config from IoTConnect: region=%s channel=%s endpoint=%s role=%s thing=%s",
                 pcKvsAwsRegion, pcKvsChannelName, pcKvsCredentialsEndpoint,
                 pcKvsIotRoleAlias, pcKvsIotThingName );

        ( void ) xEventGroupSetBits( xSystemEvents, EVT_MASK_IOTC_KVS_CONFIG );
    }
    else
    {
        LogWarn( "Could not fully parse KVS config from IoTConnect identity vs block." );
    }

done:
    cJSON_Delete( pxRoot );
}

static BaseType_t prvRunIdentityFlow( const char * pcCpid,
                                      const char * pcEnv,
                                      const char * pcDuid,
                                      char ** ppcIdentityJson )
{
    BaseType_t xStatus = pdFALSE;
    IotclDraUrlContext xDiscoveryUrl = { 0 };
    IotclDraUrlContext xIdentityUrl = { 0 };
    char * pcDiscoveryResponse = NULL;
    char * pcIdentityResponse = NULL;
    int lStatus = IOTCL_ERR_FAILED;
    int lIdentitySlack = ( int ) ( strlen( IOTCL_DRA_IDENTITY_PREFIX ) + strlen( pcDuid ) );

    *ppcIdentityJson = NULL;

    if( xIoTConnectRuntime.xCloud == IOTCONNECT_CLOUD_AZURE )
    {
        lStatus = iotcl_dra_discovery_init_url_azure( &xDiscoveryUrl, pcCpid, pcEnv );
    }
    else
    {
        lStatus = iotcl_dra_discovery_init_url_aws( &xDiscoveryUrl, pcCpid, pcEnv );
    }

    if( lStatus != IOTCL_SUCCESS )
    {
        goto cleanup;
    }

    if( prvHttpGetJson( &xDiscoveryUrl,
                        TLS_IOTCONNECT_DRA_CA_CERT_LABEL,
                        &pcDiscoveryResponse ) == pdFALSE )
    {
        goto cleanup;
    }

    lStatus = iotcl_dra_discovery_parse( &xIdentityUrl,
                                         lIdentitySlack,
                                         pcDiscoveryResponse );

    if( lStatus != IOTCL_SUCCESS )
    {
        LogError( "Failed to parse IOTCONNECT discovery response." );
        goto cleanup;
    }

    lStatus = iotcl_dra_identity_build_url( &xIdentityUrl, pcDuid );

    if( lStatus != IOTCL_SUCCESS )
    {
        goto cleanup;
    }

    if( prvHttpGetJson( &xIdentityUrl,
                        TLS_IOTCONNECT_DRA_CA_CERT_LABEL,
                        &pcIdentityResponse ) == pdFALSE )
    {
        goto cleanup;
    }

    lStatus = iotcl_dra_identity_configure_library_mqtt( pcIdentityResponse );

    if( lStatus != IOTCL_SUCCESS )
    {
        LogError( "Failed to parse IOTCONNECT identity response." );
        goto cleanup;
    }

    prvApplyAwsIdentityWorkaround();
    *ppcIdentityJson = pcIdentityResponse;
    pcIdentityResponse = NULL;
    xStatus = pdTRUE;

cleanup:
    if( pcDiscoveryResponse != NULL )
    {
        vPortFree( pcDiscoveryResponse );
    }

    if( pcIdentityResponse != NULL )
    {
        vPortFree( pcIdentityResponse );
    }

    iotcl_dra_url_deinit( &xDiscoveryUrl );
    iotcl_dra_url_deinit( &xIdentityUrl );

    return xStatus;
}

static BaseType_t prvBootstrapIoTConnect( void )
{
    BaseType_t xStatus = pdFALSE;
    char * pcCpid = NULL;
    char * pcEnv = NULL;
    char * pcDuid = NULL;
    char * pcIdentityJson = NULL;
    IotclClientConfig xClientConfig;
    IotclEventConfig xEvents = { 0 };

    prvResetRuntimeState();
    iotcl_configure_dynamic_memory( pvPortMalloc, vPortFree );

    if( xIoTConnectRuntime.xEventQueue == NULL )
    {
        xIoTConnectRuntime.xEventQueue = xQueueCreate( IOTCONNECT_EVENT_QUEUE_LENGTH,
                                                       sizeof( IoTConnectQueuedEvent_t ) );

        if( xIoTConnectRuntime.xEventQueue == NULL )
        {
            LogError( "Failed to create IOTCONNECT runtime queue." );
            return pdFALSE;
        }
    }
    else
    {
        ( void ) xQueueReset( xIoTConnectRuntime.xEventQueue );
    }

    if( prvReadConfigStrings( &pcCpid, &pcEnv, &pcDuid ) == pdFALSE )
    {
        goto cleanup;
    }

    iotcl_init_client_config( &xClientConfig );
    xClientConfig.device.instance_type = IOTCL_DCT_CUSTOM;
    xClientConfig.device.cpid = pcCpid;
    xClientConfig.device.duid = pcDuid;
    xClientConfig.mqtt_send_cb = prvIoTConnectMqttSendCallback;

    if( xIoTConnectRuntime.xAppMode == IOTCONNECT_APP_MODE_SAMPLE )
    {
        xEvents.cmd_cb = prvSampleCommandCallback;
        xEvents.ota_cb = prvSampleOtaCallback;
    }
    else
    {
        xEvents.cmd_cb = prvDemoCommandCallback;
        xEvents.ota_cb = prvDemoOtaCallback;
    }

    xClientConfig.events = xEvents;

    if( iotcl_init( &xClientConfig ) != IOTCL_SUCCESS )
    {
        LogError( "Failed to initialize IOTCONNECT client library." );
        goto cleanup;
    }

    if( prvTryLoadCachedIdentity() == pdFALSE )
    {
        if( prvRunIdentityFlow( pcCpid, pcEnv, pcDuid, &pcIdentityJson ) == pdFALSE )
        {
            goto cleanup;
        }

        ( void ) prvSaveCachedIdentity( pcIdentityJson );
    }

    xIoTConnectRuntime.xReady = pdTRUE;

    if( iotcl_mqtt_get_config() != NULL )
    {
        iotcl_mqtt_print_config();
    }

    /* Parse the "vs" block for KVS WebRTC config (if present) */
    if( pcIdentityJson != NULL )
    {
        prvParseIdentityKvsConfig( pcIdentityJson );
    }

    xStatus = pdTRUE;

cleanup:
    if( pcIdentityJson != NULL )
    {
        vPortFree( pcIdentityJson );
    }

    if( xStatus == pdFALSE )
    {
        iotcl_deinit();
    }

    if( pcCpid != NULL )
    {
        vPortFree( pcCpid );
    }

    if( pcEnv != NULL )
    {
        vPortFree( pcEnv );
    }

    if( pcDuid != NULL )
    {
        vPortFree( pcDuid );
    }

    return xStatus;
}

static void prvMqttPublishCommandCallback( MQTTAgentCommandContext_t * pxCommandContext,
                                           MQTTAgentReturnInfo_t * pxReturnInfo )
{
    if( ( pxCommandContext != NULL ) &&
        ( pxCommandContext->xTaskToNotify != NULL ) )
    {
        xTaskNotify( pxCommandContext->xTaskToNotify,
                     pxReturnInfo->returnCode,
                     eSetValueWithOverwrite );
    }
}

MQTTStatus_t xIoTConnectPublishPayload( const char * pcTopic,
                                        const char * pcPayload,
                                        MQTTQoS_t xQoS,
                                        bool xRetain )
{
    MQTTStatus_t xMqttStatus = MQTTSuccess;
    MQTTAgentCommandInfo_t xCommandParams = { 0 };
    MQTTPublishInfo_t xPublishInfo = { 0 };
    MQTTAgentCommandContext_t xCommandContext = { 0 };
    MQTTAgentHandle_t xAgentHandle = xGetMqttAgentHandle();
    uint32_t ulNotifyValue = 0U;

    if( ( pcTopic == NULL ) || ( pcPayload == NULL ) )
    {
        return MQTTBadParameter;
    }

    if( ( xAgentHandle == NULL ) || ( xIsMqttAgentConnected() == false ) )
    {
        return MQTTKeepAliveTimeout;
    }

    xTaskNotifyStateClear( NULL );

    xPublishInfo.qos = xQoS;
    xPublishInfo.retain = xRetain;
    xPublishInfo.pTopicName = pcTopic;
    xPublishInfo.topicNameLength = ( uint16_t ) strlen( pcTopic );
    xPublishInfo.pPayload = pcPayload;
    xPublishInfo.payloadLength = strlen( pcPayload );

    xCommandContext.xTaskToNotify = xTaskGetCurrentTaskHandle();

    xCommandParams.blockTimeMs = 500U;
    xCommandParams.cmdCompleteCallback = prvMqttPublishCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = &xCommandContext;

    do
    {
        xMqttStatus = MQTTAgent_Publish( xAgentHandle,
                                         &xPublishInfo,
                                         &xCommandParams );

        if( xMqttStatus == MQTTSuccess )
        {
            if( xTaskNotifyWait( 0U,
                                 0U,
                                 &ulNotifyValue,
                                 portMAX_DELAY ) == pdTRUE )
            {
                xMqttStatus = ( ulNotifyValue == 0U ) ? MQTTSuccess : MQTTSendFailed;
            }
            else
            {
                xMqttStatus = MQTTSendFailed;
            }
        }
    } while( xMqttStatus != MQTTSuccess );

    return xMqttStatus;
}

static void prvIoTConnectMqttSendCallback( const char * pcTopic,
                                           const char * pcJsonStr )
{
    MQTTStatus_t xStatus = xIoTConnectPublishPayload( pcTopic,
                                                      pcJsonStr,
                                                      MQTTQoS1,
                                                      false );

    if( xStatus != MQTTSuccess )
    {
        LogWarn( "IOTCONNECT publish failed for topic %s with status %s.",
                 pcTopic,
                 MQTT_Status_strerror( xStatus ) );
    }
}

static BaseType_t prvSubscribeToC2DTopic( MQTTAgentHandle_t xMqttHandle )
{
    MQTTStatus_t xMqttStatus = MQTTSuccess;
    const char * pcTopicFilter = NULL;
    IotclMqttConfig * pxMqttConfig = iotcl_mqtt_get_config();

    if( ( xMqttHandle == NULL ) || ( pxMqttConfig == NULL ) )
    {
        return pdFALSE;
    }

    pcTopicFilter = pxMqttConfig->sub_c2d;

    if( pcTopicFilter == NULL )
    {
        LogError( "IOTCONNECT C2D topic is not configured." );
        return pdFALSE;
    }

    do
    {
        xMqttStatus = MqttAgent_SubscribeSync( xMqttHandle,
                                               pcTopicFilter,
                                               MQTTQoS1,
                                               prvC2DIncomingPublishCallback,
                                               NULL );

        if( xMqttStatus != MQTTSuccess )
        {
            LogWarn( "Failed to subscribe to IOTCONNECT C2D topic. Retrying." );
            vTaskDelay( pdMS_TO_TICKS( 1000U ) );
        }
    } while( xMqttStatus != MQTTSuccess );

    return pdTRUE;
}

static void prvC2DIncomingPublishCallback( void * pvIncomingPublishCallbackContext,
                                           MQTTPublishInfo_t * pxPublishInfo )
{
    char pcTopic[ IOTCONNECT_TOPIC_BUFFER_LEN ];

    ( void ) pvIncomingPublishCallbackContext;

    if( ( pxPublishInfo == NULL ) ||
        ( pxPublishInfo->pTopicName == NULL ) ||
        ( pxPublishInfo->topicNameLength >= sizeof( pcTopic ) ) )
    {
        LogError( "Received invalid IOTCONNECT incoming publish callback." );
        return;
    }

    memcpy( pcTopic,
            pxPublishInfo->pTopicName,
            pxPublishInfo->topicNameLength );
    pcTopic[ pxPublishInfo->topicNameLength ] = '\0';

    ( void ) iotcl_mqtt_receive_with_length( pcTopic,
                                             ( const uint8_t * ) pxPublishInfo->pPayload,
                                             pxPublishInfo->payloadLength );
}

static void prvDemoButtonEdgeCallback( void * pvContext )
{
    ( void ) pvContext;

    if( xIoTConnectRuntime.xButtonEvents != NULL )
    {
        ( void ) xEventGroupSetBitsFromISR( xIoTConnectRuntime.xButtonEvents,
                                            IOTCONNECT_DEMO_BUTTON_EVENT,
                                            NULL );
    }
}

static BaseType_t prvReadUserButtonPressed( void )
{
    return ( BaseType_t ) ( HAL_GPIO_ReadPin( USER_Button_GPIO_Port,
                                              USER_Button_Pin ) == USER_BUTTON_ON );
}

static void prvRefreshLedStateFromHardware( void )
{
    xIoTConnectRuntime.xLedRedOn = ( BaseType_t ) ( HAL_GPIO_ReadPin( LED_RED_GPIO_Port, LED_RED_Pin ) == LED_RED_ON );
    xIoTConnectRuntime.xLedGreenOn = ( BaseType_t ) ( HAL_GPIO_ReadPin( LED_GREEN_GPIO_Port, LED_GREEN_Pin ) == LED_GREEN_ON );
}

static void prvSetLedState( uint8_t ucLedMask,
                            uint8_t ucLedValueMask )
{
    if( ( ucLedMask & IOTCONNECT_LED_RED_MASK ) != 0U )
    {
        xIoTConnectRuntime.xLedRedOn = ( BaseType_t ) ( ( ucLedValueMask & IOTCONNECT_LED_RED_MASK ) != 0U );
        HAL_GPIO_WritePin( LED_RED_GPIO_Port,
                           LED_RED_Pin,
                           xIoTConnectRuntime.xLedRedOn ? LED_RED_ON : LED_RED_OFF );
    }

    if( ( ucLedMask & IOTCONNECT_LED_GREEN_MASK ) != 0U )
    {
        xIoTConnectRuntime.xLedGreenOn = ( BaseType_t ) ( ( ucLedValueMask & IOTCONNECT_LED_GREEN_MASK ) != 0U );
        HAL_GPIO_WritePin( LED_GREEN_GPIO_Port,
                           LED_GREEN_Pin,
                           xIoTConnectRuntime.xLedGreenOn ? LED_GREEN_ON : LED_GREEN_OFF );
    }
}

static BaseType_t prvPublishDemoTelemetry( void )
{
    BaseType_t xStatus = pdFALSE;
    IotclMessageHandle xMessage = iotcl_telemetry_create();

    if( xMessage == NULL )
    {
        return pdFALSE;
    }

    ( void ) iotcl_telemetry_set_string( xMessage, "mode", "demo" );
    ( void ) iotcl_telemetry_set_string( xMessage, "firmware_version", IOTCONNECT_APP_VERSION );
    ( void ) iotcl_telemetry_set_bool( xMessage, "led_red", xIoTConnectRuntime.xLedRedOn == pdTRUE );
    ( void ) iotcl_telemetry_set_bool( xMessage, "led_green", xIoTConnectRuntime.xLedGreenOn == pdTRUE );
    ( void ) iotcl_telemetry_set_bool( xMessage, "button_user", xIoTConnectRuntime.xButtonPressed == pdTRUE );

    /* Phase 3: NPU people-detection stats (only once the network is up;
     * no-ops in non-AI builds).  The platform template needs matching
     * NUMBER attributes: ai_people, ai_top_conf, ai_infer_ms.           */
    {
        int32_t lDetections = 0;
        uint32_t ulTopConfPct = 0U, ulInferMs = 0U;

        if( AiDetection_GetTelemetry( &lDetections, &ulTopConfPct, &ulInferMs, NULL ) != 0U )
        {
            ( void ) iotcl_telemetry_set_number( xMessage, "ai_people", ( double ) lDetections );
            ( void ) iotcl_telemetry_set_number( xMessage, "ai_top_conf", ( double ) ulTopConfPct );
            ( void ) iotcl_telemetry_set_number( xMessage, "ai_infer_ms", ( double ) ulInferMs );
        }
    }

    if( iotcl_mqtt_send_telemetry( xMessage, false ) == IOTCL_SUCCESS )
    {
        xStatus = pdTRUE;
    }
    else
    {
        LogWarn( "Failed to publish IOTCONNECT demo telemetry." );
    }

    iotcl_telemetry_destroy( xMessage );
    return xStatus;
}

static BaseType_t prvPublishSampleTelemetry( void )
{
    BaseType_t xStatus = pdFALSE;
    double dTickSeed = ( double ) ( HAL_GetTick() % 1000U ) / 1000.0;
    IotclMessageHandle xMessage = iotcl_telemetry_create();

    if( xMessage == NULL )
    {
        return pdFALSE;
    }

    ( void ) iotcl_telemetry_set_string( xMessage, "mode", "sample" );
    ( void ) iotcl_telemetry_set_string( xMessage, "version", IOTCONNECT_APP_VERSION );
    ( void ) iotcl_telemetry_set_number( xMessage, "random_int", ( double ) ( HAL_GetTick() % 10U ) );
    ( void ) iotcl_telemetry_set_number( xMessage, "random_decimal", dTickSeed );
    ( void ) iotcl_telemetry_set_bool( xMessage, "random_boolean", ( HAL_GetTick() & 1U ) != 0U );
    ( void ) iotcl_telemetry_set_number( xMessage, "coordinate.x", dTickSeed * 10.0 );
    ( void ) iotcl_telemetry_set_number( xMessage, "coordinate.y", ( 1.0 - dTickSeed ) * 10.0 );

    if( iotcl_mqtt_send_telemetry( xMessage, false ) == IOTCL_SUCCESS )
    {
        xStatus = pdTRUE;
    }
    else
    {
        LogWarn( "Failed to publish IOTCONNECT sample telemetry." );
    }

    iotcl_telemetry_destroy( xMessage );
    return xStatus;
}

static void prvQueueEvent( const IoTConnectQueuedEvent_t * pxEvent )
{
    if( ( xIoTConnectRuntime.xEventQueue != NULL ) &&
        ( pxEvent != NULL ) )
    {
        if( xQueueSendToBack( xIoTConnectRuntime.xEventQueue,
                              pxEvent,
                              0U ) != pdTRUE )
        {
            LogWarn( "IOTCONNECT event queue is full. Dropping event." );
        }
    }
}

static void prvQueueCmdAckEvent( const char * pcAckId,
                                 int lStatus,
                                 const char * pcMessage )
{
    IoTConnectQueuedEvent_t xEvent = { 0 };

    xEvent.xType = IOTCONNECT_EVENT_CMD_ACK;
    xEvent.lStatus = lStatus;
    prvCopyString( xEvent.pcAckId, sizeof( xEvent.pcAckId ), pcAckId );
    prvCopyString( xEvent.pcMessage, sizeof( xEvent.pcMessage ), pcMessage );
    prvQueueEvent( &xEvent );
}

static void prvQueueOtaAckEvent( const char * pcAckId,
                                 int lStatus,
                                 const char * pcMessage )
{
    IoTConnectQueuedEvent_t xEvent = { 0 };

    xEvent.xType = IOTCONNECT_EVENT_OTA_ACK;
    xEvent.lStatus = lStatus;
    prvCopyString( xEvent.pcAckId, sizeof( xEvent.pcAckId ), pcAckId );
    prvCopyString( xEvent.pcMessage, sizeof( xEvent.pcMessage ), pcMessage );
    prvQueueEvent( &xEvent );
}

static BaseType_t prvParseDemoLedCommand( const char * pcCommand,
                                          uint8_t * pucLedMask,
                                          uint8_t * pucLedValueMask )
{
    *pucLedMask = 0U;
    *pucLedValueMask = 0U;

    if( pcCommand == NULL )
    {
        return pdFALSE;
    }

    if( strcmp( pcCommand, "LED_RED_ON" ) == 0 )
    {
        *pucLedMask = IOTCONNECT_LED_RED_MASK;
        *pucLedValueMask = IOTCONNECT_LED_RED_MASK;
    }
    else if( strcmp( pcCommand, "LED_RED_OFF" ) == 0 )
    {
        *pucLedMask = IOTCONNECT_LED_RED_MASK;
    }
    else if( strcmp( pcCommand, "LED_GREEN_ON" ) == 0 )
    {
        *pucLedMask = IOTCONNECT_LED_GREEN_MASK;
        *pucLedValueMask = IOTCONNECT_LED_GREEN_MASK;
    }
    else if( strcmp( pcCommand, "LED_GREEN_OFF" ) == 0 )
    {
        *pucLedMask = IOTCONNECT_LED_GREEN_MASK;
    }
    else if( strcmp( pcCommand, "LED_ALL_ON" ) == 0 )
    {
        *pucLedMask = IOTCONNECT_LED_RED_MASK | IOTCONNECT_LED_GREEN_MASK;
        *pucLedValueMask = *pucLedMask;
    }
    else if( strcmp( pcCommand, "LED_ALL_OFF" ) == 0 )
    {
        *pucLedMask = IOTCONNECT_LED_RED_MASK | IOTCONNECT_LED_GREEN_MASK;
    }
    else
    {
        return pdFALSE;
    }

    return pdTRUE;
}

static void prvDemoCommandCallback( IotclC2dEventData xEventData )
{
    const char * pcCommand = iotcl_c2d_get_command( xEventData );
    const char * pcAckId = iotcl_c2d_get_ack_id( xEventData );
    IoTConnectQueuedEvent_t xEvent = { 0 };

    if( pcCommand == NULL )
    {
        prvQueueCmdAckEvent( pcAckId,
                             IOTCL_C2D_EVT_CMD_FAILED,
                             "Invalid command payload" );
        return;
    }

    xEvent.xType = IOTCONNECT_EVENT_DEMO_LED_UPDATE;
    xEvent.lStatus = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
    prvCopyString( xEvent.pcAckId, sizeof( xEvent.pcAckId ), pcAckId );
    prvCopyString( xEvent.pcMessage, sizeof( xEvent.pcMessage ), "OK" );

    if( prvParseDemoLedCommand( pcCommand,
                                &( xEvent.ucLedMask ),
                                &( xEvent.ucLedValueMask ) ) == pdFALSE )
    {
        prvQueueCmdAckEvent( pcAckId,
                             IOTCL_C2D_EVT_CMD_FAILED,
                             "Unknown command" );
        return;
    }

    prvQueueEvent( &xEvent );
}

static void prvDemoOtaCallback( IotclC2dEventData xEventData )
{
    prvQueueOtaAckEvent( iotcl_c2d_get_ack_id( xEventData ),
                         IOTCL_C2D_EVT_OTA_FAILED,
                         "Demo mode does not implement OTA" );
}

static void prvSampleCommandCallback( IotclC2dEventData xEventData )
{
    prvQueueCmdAckEvent( iotcl_c2d_get_ack_id( xEventData ),
                         IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK,
                         "Not implemented" );
}

static void prvSampleOtaCallback( IotclC2dEventData xEventData )
{
    const char * pcAckId = iotcl_c2d_get_ack_id( xEventData );
    const char * pcVersion = iotcl_c2d_get_ota_sw_version( xEventData );
    int lStatus = IOTCL_C2D_EVT_OTA_DOWNLOAD_FAILED;
    const char * pcMessage = "Not implemented";

    if( ( pcVersion != NULL ) &&
        ( strcmp( pcVersion, IOTCONNECT_APP_VERSION ) == 0 ) )
    {
        lStatus = IOTCL_C2D_EVT_OTA_DOWNLOAD_DONE;
        pcMessage = "Version is matching";
    }
    else if( ( pcVersion != NULL ) &&
             ( strcmp( pcVersion, IOTCONNECT_APP_VERSION ) < 0 ) )
    {
        lStatus = IOTCL_C2D_EVT_OTA_FAILED;
        pcMessage = "Device firmware version is newer";
    }

    prvQueueOtaAckEvent( pcAckId, lStatus, pcMessage );
}

static BaseType_t prvHandleQueuedAckEvent( const IoTConnectQueuedEvent_t * pxEvent )
{
    if( ( pxEvent == NULL ) || ( pxEvent->pcAckId[ 0 ] == '\0' ) )
    {
        return pdTRUE;
    }

    if( pxEvent->xType == IOTCONNECT_EVENT_OTA_ACK )
    {
        return ( BaseType_t ) ( iotcl_mqtt_send_ota_ack( pxEvent->pcAckId,
                                                         pxEvent->lStatus,
                                                         pxEvent->pcMessage ) == IOTCL_SUCCESS );
    }

    return ( BaseType_t ) ( iotcl_mqtt_send_cmd_ack( pxEvent->pcAckId,
                                                     pxEvent->lStatus,
                                                     pxEvent->pcMessage ) == IOTCL_SUCCESS );
}

static BaseType_t prvHandleQueuedCommandEvent( const IoTConnectQueuedEvent_t * pxEvent,
                                               BaseType_t * pxPublishTelemetry )
{
    if( ( pxEvent == NULL ) || ( pxPublishTelemetry == NULL ) )
    {
        return pdFALSE;
    }

    if( pxEvent->xType == IOTCONNECT_EVENT_DEMO_LED_UPDATE )
    {
        prvSetLedState( pxEvent->ucLedMask, pxEvent->ucLedValueMask );

        if( pxEvent->pcAckId[ 0 ] != '\0' )
        {
            ( void ) prvHandleQueuedAckEvent( pxEvent );
        }

        *pxPublishTelemetry = pdTRUE;
    }
    else
    {
        ( void ) prvHandleQueuedAckEvent( pxEvent );
    }

    return pdTRUE;
}

static void vIoTConnectDemoTask( void * pvParameters )
{
    MQTTAgentHandle_t xMqttHandle = NULL;
    IoTConnectQueuedEvent_t xEvent = { 0 };
    BaseType_t xPublishTelemetry = pdTRUE;

    ( void ) pvParameters;

    vSleepUntilMQTTAgentReady();
    vSleepUntilMQTTAgentConnected();
    xMqttHandle = xGetMqttAgentHandle();
    configASSERT( xMqttHandle != NULL );

    if( xIoTConnectRuntime.xButtonEvents == NULL )
    {
        xIoTConnectRuntime.xButtonEvents = xEventGroupCreate();
        configASSERT( xIoTConnectRuntime.xButtonEvents != NULL );
    }
    else
    {
        ( void ) xEventGroupClearBits( xIoTConnectRuntime.xButtonEvents,
                                       IOTCONNECT_DEMO_BUTTON_EVENT );
    }

    if( prvSubscribeToC2DTopic( xMqttHandle ) == pdFALSE )
    {
        vTaskDelete( NULL );
    }

    prvRefreshLedStateFromHardware();
    xIoTConnectRuntime.xButtonPressed = prvReadUserButtonPressed();

    GPIO_EXTI_Register_Rising_Callback( USER_Button_Pin,
                                        prvDemoButtonEdgeCallback,
                                        NULL );
    GPIO_EXTI_Register_Falling_Callback( USER_Button_Pin,
                                         prvDemoButtonEdgeCallback,
                                         NULL );
    ( void ) xEventGroupSetBits( xIoTConnectRuntime.xButtonEvents,
                                 IOTCONNECT_DEMO_BUTTON_EVENT );

    TickType_t xLastPublish = xTaskGetTickCount();

    for( ;; )
    {
        if( xQueueReceive( xIoTConnectRuntime.xEventQueue,
                           &xEvent,
                           pdMS_TO_TICKS( IOTCONNECT_DEMO_POLL_INTERVAL_MS ) ) == pdTRUE )
        {
            ( void ) prvHandleQueuedCommandEvent( &xEvent, &xPublishTelemetry );
        }

        if( xEventGroupWaitBits( xIoTConnectRuntime.xButtonEvents,
                                 IOTCONNECT_DEMO_BUTTON_EVENT,
                                 pdTRUE,
                                 pdFALSE,
                                 0U ) != 0U )
        {
            xIoTConnectRuntime.xButtonPressed = prvReadUserButtonPressed();
            xPublishTelemetry = pdTRUE;
        }

        /* Heartbeat: publish on a fixed period so live NPU detection stats
         * (and LED/button state) stream to IOTCONNECT even with no command
         * or button activity. */
        if( ( xTaskGetTickCount() - xLastPublish ) >=
            pdMS_TO_TICKS( IOTCONNECT_DEMO_PUBLISH_PERIOD_MS ) )
        {
            xPublishTelemetry = pdTRUE;
        }

        if( xPublishTelemetry == pdTRUE )
        {
            ( void ) prvPublishDemoTelemetry();
            xPublishTelemetry = pdFALSE;
            xLastPublish      = xTaskGetTickCount();
        }
    }
}

static void vIoTConnectSampleTask( void * pvParameters )
{
    MQTTAgentHandle_t xMqttHandle = NULL;
    IoTConnectQueuedEvent_t xEvent = { 0 };
    BaseType_t xTelemetryPending = pdTRUE;

    ( void ) pvParameters;

    vSleepUntilMQTTAgentReady();
    vSleepUntilMQTTAgentConnected();
    xMqttHandle = xGetMqttAgentHandle();
    configASSERT( xMqttHandle != NULL );

    if( prvSubscribeToC2DTopic( xMqttHandle ) == pdFALSE )
    {
        vTaskDelete( NULL );
    }

    for( ;; )
    {
        if( xQueueReceive( xIoTConnectRuntime.xEventQueue,
                           &xEvent,
                           pdMS_TO_TICKS( IOTCONNECT_SAMPLE_PERIOD_MS ) ) == pdTRUE )
        {
            ( void ) prvHandleQueuedAckEvent( &xEvent );
        }
        else
        {
            xTelemetryPending = pdTRUE;
        }

        if( xTelemetryPending == pdTRUE )
        {
            ( void ) prvPublishSampleTelemetry();
            xTelemetryPending = pdFALSE;
        }
    }
}

void vIoTConnectStartupTask( void * pvParameters )
{
    BaseType_t xTaskResult = pdFALSE;

    ( void ) pvParameters;

    for( ;; )
    {
        ( void ) xEventGroupWaitBits( xSystemEvents,
                                      EVT_MASK_NET_CONNECTED,
                                      pdFALSE,
                                      pdTRUE,
                                      portMAX_DELAY );

        if( prvBootstrapIoTConnect() == pdTRUE )
        {
            xTaskResult = xTaskCreate( vMQTTAgentTask,
                                       "MQTTAgent",
                                       TASK_STACK_SIZE_MQTT_AGENT,
                                       NULL,
                                       TASK_PRIO_MQTTA_AGENT,
                                       NULL );
            configASSERT( xTaskResult == pdTRUE );

            if( xIoTConnectRuntime.xAppMode == IOTCONNECT_APP_MODE_SAMPLE )
            {
                xTaskResult = xTaskCreate( vIoTConnectSampleTask,
                                           "IoTCApp",
                                           TASK_STACK_SIZE_IOTCONNECT,
                                           NULL,
                                           TASK_PRIO_IOTCONNECT,
                                           NULL );
            }
            else
            {
                xTaskResult = xTaskCreate( vIoTConnectDemoTask,
                                           "IoTCApp",
                                           TASK_STACK_SIZE_IOTCONNECT,
                                           NULL,
                                           TASK_PRIO_IOTCONNECT,
                                           NULL );
            }

            configASSERT( xTaskResult == pdTRUE );
            vTaskDelete( NULL );
        }

        LogWarn( "IOTCONNECT bootstrap failed. Retrying in 5 seconds." );
        vTaskDelay( pdMS_TO_TICKS( 5000U ) );
    }
}
