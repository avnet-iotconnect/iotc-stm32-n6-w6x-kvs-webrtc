/*
 * sntp_port.c — STM32 implementation of the Ameba-style sntp_get_lasttime().
 *
 * The KVS WebRTC networking_utils.c calls sntp_get_lasttime() to obtain the
 * current wall-clock time and a matching FreeRTOS tick count so it can
 * extrapolate a precise timestamp between NTP syncs.
 *
 * sntp_init() performs a one-shot UDP NTP query against pool.ntp.org (or
 * the first reachable server) and stores the result via _settimeofday_epoch()
 * in syscalls.c, which makes _gettimeofday() return real wall-clock time
 * for the rest of the session.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "sntp/sntp.h"
#include "freertos_hooks.h"    /* vPetWatchdog() */

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"

#include "kvstore.h"    /* CS_NTP_HOST / CS_NTP_PORT overrides */

/* Provided by syscalls.c */
extern void _settimeofday_epoch( uint32_t sec, uint32_t usec );

/* ── NTP constants ─────────────────────────────────────────────────────── */
#define NTP_PORT            123
#define NTP_PACKET_SIZE     48
#define NTP_UNIX_OFFSET     2208988800UL   /* seconds between 1900 and 1970 */
#define NTP_RECV_TIMEOUT_MS 3000

/* NTP servers to try, in order */
static const char * const ppcNtpServers[] =
{
    "pool.ntp.org",
    "time.aws.com",
    "time.google.com",
};
#define NTP_SERVER_COUNT  ( sizeof( ppcNtpServers ) / sizeof( ppcNtpServers[0] ) )

/* HTTP servers for Date-header time sync (fallback when UDP/NTP is blocked) */
static const char * const ppcHttpServers[] =
{
    "www.google.com",
    "www.amazon.com",
    "www.microsoft.com",
};
#define HTTP_SERVER_COUNT  ( sizeof( ppcHttpServers ) / sizeof( ppcHttpServers[0] ) )

/* Raw UART for debug — bounded spin, see rationale in kvs_webrtc_task.c. */
extern void vPetWatchdog( void );
static inline void ntp_raw_putc( char c )
{
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
static void ntp_raw_puts( const char *s ) { while( *s ) ntp_raw_putc( *s++ ); }

/**
 * @brief Attempt a single NTP query against one server.
 * @return 0 on success (epoch set), -1 on failure.
 */
static void ntp_raw_int( int v )
{
    char dbuf[12];
    int  di = 0;
    if( v < 0 ) { ntp_raw_putc( '-' ); v = -v; }
    if( v == 0 ) { ntp_raw_putc( '0' ); return; }
    while( v ) { dbuf[di++] = '0' + (v % 10); v /= 10; }
    while( di-- ) ntp_raw_putc( dbuf[di] );
}

/* @param ulAddrBe  When nonzero: query this IPv4 address (network byte
 *                  order) directly, skipping DNS.  Used for the gateway-
 *                  first attempt (routers that intercept WAN NTP usually
 *                  answer port 123 themselves).
 * @param usPort    Destination UDP port (123 unless the persisted
 *                  "ntp_port" conf key overrides it). */
static int prvNtpQuery( const char * pcServer,
                        uint32_t ulAddrBe,
                        uint16_t usPort )
{
    struct sockaddr_in xAddr;
    int               fd = -1;
    uint8_t           pkt[ NTP_PACKET_SIZE ];
    int               ret = -1;
    int               n;

    ntp_raw_puts( "[NTP] resolve " );
    ntp_raw_puts( pcServer );
    ntp_raw_puts( "\r\n" );

    if( ulAddrBe != 0U )
    {
        memset( &xAddr, 0, sizeof( xAddr ) );
        xAddr.sin_family      = AF_INET;
        xAddr.sin_addr.s_addr = ulAddrBe;
    }
    else
    {
        /* Use lwIP DNS to resolve hostname to IP address */
        struct addrinfo hints, *res = NULL;
        memset( &hints, 0, sizeof( hints ) );
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        if( lwip_getaddrinfo( pcServer, "123", &hints, &res ) != 0 || res == NULL )
        {
            ntp_raw_puts( "[NTP] dns fail\r\n" );
            return -1;
        }

        /* Extract the resolved IPv4 address */
        memcpy( &xAddr, res->ai_addr, sizeof( xAddr ) );
        lwip_freeaddrinfo( res );
    }

    xAddr.sin_port = lwip_htons( usPort );
    ntp_raw_puts( "[NTP] port " );
    ntp_raw_int( ( int ) usPort );
    ntp_raw_puts( "\r\n" );

    fd = lwip_socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if( fd < 0 )
    {
        ntp_raw_puts( "[NTP] sock fail\r\n" );
        return -1;
    }
    ntp_raw_puts( "[NTP] fd=" );
    ntp_raw_int( fd );
    ntp_raw_puts( "\r\n" );

    /* Bind to ephemeral local port */
    {
        struct sockaddr_in xLocal;
        memset( &xLocal, 0, sizeof( xLocal ) );
        xLocal.sin_family = AF_INET;
        xLocal.sin_port   = 0;  /* let lwIP pick a port */
        xLocal.sin_addr.s_addr = INADDR_ANY;
        if( lwip_bind( fd, (struct sockaddr *)&xLocal, sizeof( xLocal ) ) != 0 )
        {
            ntp_raw_puts( "[NTP] bind fail e=" );
            ntp_raw_int( errno );
            ntp_raw_puts( "\r\n" );
            goto cleanup;
        }
    }

    /* Build NTP request: LI=0, VN=3, Mode=3 (client) → byte 0 = 0x1B */
    memset( pkt, 0, sizeof( pkt ) );
    pkt[0] = 0x1B;

    /* Send to the NTP server */
    n = lwip_sendto( fd, pkt, sizeof( pkt ), 0,
                     (struct sockaddr *)&xAddr, sizeof( xAddr ) );
    if( n != sizeof( pkt ) )
    {
        ntp_raw_puts( "[NTP] send fail n=" );
        ntp_raw_int( n );
        ntp_raw_puts( " e=" );
        ntp_raw_int( errno );
        ntp_raw_puts( "\r\n" );
        goto cleanup;
    }

    ntp_raw_puts( "[NTP] sent, waiting...\r\n" );

    /* Wait for response using select() — SO_RCVTIMEO doesn't work reliably */
    {
        fd_set rfds;
        struct timeval tv;
        int sel;

        FD_ZERO( &rfds );
        FD_SET( fd, &rfds );
        tv.tv_sec  = NTP_RECV_TIMEOUT_MS / 1000;
        tv.tv_usec = ( NTP_RECV_TIMEOUT_MS % 1000 ) * 1000;

        sel = lwip_select( fd + 1, &rfds, NULL, NULL, &tv );
        if( sel <= 0 )
        {
            ntp_raw_puts( "[NTP] select timeout/err s=" );
            ntp_raw_int( sel );
            ntp_raw_puts( " e=" );
            ntp_raw_int( errno );
            ntp_raw_puts( "\r\n" );
            goto cleanup;
        }
    }

    /* Data available — read it */
    {
        struct sockaddr_in xFrom;
        socklen_t xFromLen = sizeof( xFrom );
        n = lwip_recvfrom( fd, pkt, sizeof( pkt ), 0,
                           (struct sockaddr *)&xFrom, &xFromLen );
    }
    if( n < NTP_PACKET_SIZE )
    {
        ntp_raw_puts( "[NTP] recv fail n=" );
        ntp_raw_int( n );
        ntp_raw_puts( " e=" );
        ntp_raw_int( errno );
        ntp_raw_puts( "\r\n" );
        goto cleanup;
    }

    /* Extract Transmit Timestamp (bytes 40-43 = seconds since 1900-01-01) */
    {
        uint32_t ntp_sec = ( (uint32_t)pkt[40] << 24 ) |
                           ( (uint32_t)pkt[41] << 16 ) |
                           ( (uint32_t)pkt[42] <<  8 ) |
                           ( (uint32_t)pkt[43] );
        uint32_t ntp_frac = ( (uint32_t)pkt[44] << 24 ) |
                            ( (uint32_t)pkt[45] << 16 ) |
                            ( (uint32_t)pkt[46] <<  8 ) |
                            ( (uint32_t)pkt[47] );

        if( ntp_sec < NTP_UNIX_OFFSET )
        {
            ntp_raw_puts( "[NTP] bad timestamp\r\n" );
            goto cleanup;
        }

        uint32_t unix_sec  = ntp_sec - NTP_UNIX_OFFSET;
        uint32_t unix_usec = ( uint32_t )( ( (uint64_t)ntp_frac * 1000000ULL ) >> 32 );

        _settimeofday_epoch( unix_sec, unix_usec );

        ntp_raw_puts( "[NTP] time set OK\r\n" );
        ret = 0;
    }

cleanup:
    lwip_close( fd );
    return ret;
}

/* ── HTTP Date-header fallback (when UDP/NTP port 123 is blocked) ──────── */

static int prvMonthIdx( const char * s )
{
    static const char acMon[][ 4 ] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int i;
    for( i = 0; i < 12; i++ )
    {
        if( s[0] == acMon[i][0] && s[1] == acMon[i][1] && s[2] == acMon[i][2] )
            return i;
    }
    return -1;
}

/**
 * @brief Convert UTC broken-down time to Unix epoch (seconds since 1970-01-01).
 * @param mo  0-based month (0 = January).
 */
static uint32_t prvUtcToEpoch( int yr, int mo, int dy, int hr, int mn, int sc )
{
    static const int aiMdays[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int days;

    days  = ( yr - 1970 ) * 365;
    days += ( yr - 1 ) / 4   - 492;   /* leap years: floor(1969/4)   = 492 */
    days -= ( yr - 1 ) / 100 -  19;   /*             floor(1969/100) =  19 */
    days += ( yr - 1 ) / 400 -   4;   /*             floor(1969/400) =   4 */
    days += aiMdays[ mo ] + dy - 1;

    if( mo > 1 && ( ( yr % 4 == 0 && yr % 100 != 0 ) || yr % 400 == 0 ) )
        days++;

    return ( uint32_t )days * 86400U +
           ( uint32_t )hr * 3600U +
           ( uint32_t )mn * 60U +
           ( uint32_t )sc;
}

/**
 * @brief Get wall-clock time from an HTTP Date: header via plain TCP port 80.
 * @return 0 on success (epoch set), -1 on failure.
 */
static int prvHttpTimeQuery( const char * pcServer )
{
    struct sockaddr_in xAddr;
    int    fd  = -1;
    int    ret = -1;
    int    n;
    char   buf[ 512 ];

    ntp_raw_puts( "[HTTP] resolve " );
    ntp_raw_puts( pcServer );
    ntp_raw_puts( "\r\n" );

    /* DNS resolve */
    {
        struct addrinfo hints, *res = NULL;
        memset( &hints, 0, sizeof( hints ) );
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if( lwip_getaddrinfo( pcServer, "80", &hints, &res ) != 0 || res == NULL )
        {
            ntp_raw_puts( "[HTTP] dns fail\r\n" );
            return -1;
        }
        memcpy( &xAddr, res->ai_addr, sizeof( xAddr ) );
        lwip_freeaddrinfo( res );
    }

    fd = lwip_socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if( fd < 0 )
    {
        ntp_raw_puts( "[HTTP] sock fail\r\n" );
        return -1;
    }

    ntp_raw_puts( "[HTTP] connecting...\r\n" );
    if( lwip_connect( fd, (struct sockaddr *)&xAddr, sizeof( xAddr ) ) != 0 )
    {
        ntp_raw_puts( "[HTTP] connect fail e=" );
        ntp_raw_int( errno );
        ntp_raw_puts( "\r\n" );
        goto cleanup;
    }
    ntp_raw_puts( "[HTTP] connected\r\n" );

    /* Build and send HEAD request */
    {
        static const char pcPre[]  = "HEAD / HTTP/1.1\r\nHost: ";
        static const char pcPost[] = "\r\nConnection: close\r\n\r\n";
        int pos = 0;
        int sl;

        sl = sizeof( pcPre ) - 1;
        memcpy( buf, pcPre, sl );
        pos += sl;

        sl = strlen( pcServer );
        memcpy( buf + pos, pcServer, sl );
        pos += sl;

        sl = sizeof( pcPost ) - 1;
        memcpy( buf + pos, pcPost, sl );
        pos += sl;

        if( lwip_send( fd, buf, pos, 0 ) != pos )
        {
            ntp_raw_puts( "[HTTP] send fail\r\n" );
            goto cleanup;
        }
    }

    ntp_raw_puts( "[HTTP] sent, waiting...\r\n" );

    /* Wait for response with select() */
    {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO( &rfds );
        FD_SET( fd, &rfds );
        tv.tv_sec  = 5;
        tv.tv_usec = 0;
        if( lwip_select( fd + 1, &rfds, NULL, NULL, &tv ) <= 0 )
        {
            ntp_raw_puts( "[HTTP] select timeout\r\n" );
            goto cleanup;
        }
    }

    n = lwip_recv( fd, buf, sizeof( buf ) - 1, 0 );
    if( n <= 0 )
    {
        ntp_raw_puts( "[HTTP] recv fail\r\n" );
        goto cleanup;
    }
    buf[ n ] = '\0';

    /* Parse Date header — format: "Date: Wed, 09 Apr 2026 15:30:00 GMT" */
    {
        char * pDate = strstr( buf, "Date: " );
        char * p;
        int    day, yr, hr, mn, sc, mo;
        uint32_t epoch;

        if( !pDate )
            pDate = strstr( buf, "date: " );
        if( !pDate )
        {
            ntp_raw_puts( "[HTTP] no Date hdr\r\n" );
            goto cleanup;
        }

        /* Skip "Date: " and find ", " after day-name */
        p = strchr( pDate + 6, ',' );
        if( !p )
        {
            ntp_raw_puts( "[HTTP] bad Date fmt\r\n" );
            goto cleanup;
        }
        p++;                            /* skip comma */
        while( *p == ' ' ) p++;         /* skip space(s) */

        /* p → "09 Apr 2026 15:30:00 GMT" (need ≥ 20 chars) */
        if( strlen( p ) < 20 )
        {
            ntp_raw_puts( "[HTTP] Date too short\r\n" );
            goto cleanup;
        }

        /* Day (2 digits, may be space-padded) */
        if( p[0] == ' ' )
            day = p[1] - '0';
        else
            day = ( p[0] - '0' ) * 10 + ( p[1] - '0' );

        /* Month (3 chars at offset 3) */
        mo = prvMonthIdx( p + 3 );
        if( mo < 0 )
        {
            ntp_raw_puts( "[HTTP] bad month\r\n" );
            goto cleanup;
        }

        /* Year (4 digits at offset 7) */
        yr = ( p[7]  - '0' ) * 1000 + ( p[8]  - '0' ) * 100 +
             ( p[9]  - '0' ) * 10   + ( p[10] - '0' );

        /* Hour:Min:Sec at offset 12 */
        hr = ( p[12] - '0' ) * 10 + ( p[13] - '0' );
        mn = ( p[15] - '0' ) * 10 + ( p[16] - '0' );
        sc = ( p[18] - '0' ) * 10 + ( p[19] - '0' );

        epoch = prvUtcToEpoch( yr, mo, day, hr, mn, sc );

        if( epoch > 1000000000UL )
        {
            _settimeofday_epoch( epoch, 0 );
            ntp_raw_puts( "[HTTP] time set OK epoch=" );
            ntp_raw_int( (int)epoch );
            ntp_raw_puts( "\r\n" );
            ret = 0;
        }
        else
        {
            ntp_raw_puts( "[HTTP] bad epoch\r\n" );
        }
    }

cleanup:
    lwip_close( fd );
    return ret;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void sntp_init( void )
{
    int i;

    uint16_t usPort = NTP_PORT;

    ntp_raw_puts( "[NTP] sntp_init start\r\n" );

    /* Persisted overrides (CLI: `conf set ntp_port 98`, `conf set
     * ntp_host my.server`) so the working NTP setup for a given LAN never
     * has to live only in a binary again. */
    {
        BaseType_t xFound = pdFALSE;
        uint32_t   ulConfPort = KVStore_getUInt32( CS_NTP_PORT, &xFound );

        if( ( xFound == pdTRUE ) && ( ulConfPort > 0U ) && ( ulConfPort < 65536U ) )
        {
            usPort = ( uint16_t ) ulConfPort;
        }
    }

    {
        size_t xLen    = 0;
        char * pcHost  = KVStore_getStringHeap( CS_NTP_HOST, &xLen );

        if( ( pcHost != NULL ) && ( xLen > 0 ) && ( pcHost[ 0 ] != '\0' ) )
        {
            int lHostRet;

            vPetWatchdog();
            lHostRet = prvNtpQuery( pcHost, 0U, usPort );
            vPortFree( pcHost );

            if( lHostRet == 0 )
            {
                return;   /* success */
            }
        }
        else if( pcHost != NULL )
        {
            vPortFree( pcHost );
        }
    }

    /* Gateway next: LANs whose router intercepts client NTP to public
     * servers (Google Wifi/Nest — see developer.md (W6X module notes section) §1 note)
     * still answer NTP at the gateway itself.  Fast-fails elsewhere. */
    if( netif_default != NULL )
    {
        uint32_t ulGw = ip4_addr_get_u32( netif_ip4_gw( netif_default ) );

        vPetWatchdog();

        if( ( ulGw != 0U ) && ( prvNtpQuery( "gateway", ulGw, usPort ) == 0 ) )
        {
            return;   /* success */
        }
    }

    /* Try NTP first (single round — fast-fail if UDP port 123 is blocked) */
    for( i = 0; i < (int)NTP_SERVER_COUNT; i++ )
    {
        vPetWatchdog();
        if( prvNtpQuery( ppcNtpServers[i], 0U, usPort ) == 0 )
            return;   /* success */
    }

    ntp_raw_puts( "[NTP] NTP failed, trying HTTP fallback...\r\n" );

    /* Fallback: extract wall-clock time from HTTP Date: header (TCP port 80) */
    for( i = 0; i < (int)HTTP_SERVER_COUNT; i++ )
    {
        vPetWatchdog();
        if( prvHttpTimeQuery( ppcHttpServers[i] ) == 0 )
            return;   /* success */
    }

    ntp_raw_puts( "[NTP] FAILED all NTP+HTTP attempts\r\n" );
}

void sntp_get_lasttime( long long * sec,
                        long long * usec,
                        unsigned int * tick )
{
    struct timeval tv;

    /* _gettimeofday now returns real time if epoch has been set */
    extern int _gettimeofday( struct timeval *tv, void *tz );
    _gettimeofday( &tv, NULL );

    *sec  = ( long long ) tv.tv_sec;
    *usec = ( long long ) tv.tv_usec;
    *tick = ( unsigned int ) xTaskGetTickCount();
}
