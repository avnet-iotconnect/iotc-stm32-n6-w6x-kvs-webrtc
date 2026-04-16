/**
  ******************************************************************************
  * @file    net_diag.c
  * @brief   WAN reachability diagnostic for ST67W611M bug report.
  *
  * Two tests, dumped to raw UART (bypasses FreeRTOS logging) with the
  * [NETDIAG] prefix so the full output is trivial to grep from a captured
  * log and attach to the bug report.
  *
  * 1. CONFIG DUMP.  Prints the device's STA IP, netmask, and gateway.
  *    Bug-report purpose: rule out "NCP is mis-classifying WAN src IPs as
  *    LAN because the netmask is wrong or the host disagrees with the NCP
  *    about what constitutes 'LAN'."  If the netmask here is /24 and the
  *    drop pattern affects any src outside 192.168.1.0/24, the filter is
  *    operating at the expected LAN-vs-WAN boundary.
  *
  * 2. ICMP ECHO PROBE.  Sends ICMP echo requests to a small set of WAN
  *    targets (public DNS resolvers + one of the AWS TURN backends that
  *    previously dropped UDP) and waits for echo replies.  The replies
  *    arrive from WAN source IPs *via the same SPI RX path that drops
  *    WAN UDP*.  Bug-report purpose: discriminate the two hypotheses for
  *    the UDP RX blackhole:
  *
  *      a) Protocol-specific UDP filter
  *           ICMP echo replies arrive OK, WAN UDP does not.
  *           --> NCP firmware is filtering UDP, not routing/ACL.
  *
  *      b) Global routing/ACL drop
  *           Neither ICMP echo replies nor WAN UDP arrive.
  *           --> NCP firmware is dropping all non-LAN inbound traffic
  *               regardless of protocol.
  *
  * The ICMP test is the outbound-initiated mirror of the UDP test:
  * device initiates, WAN host responds, response must traverse the same
  * NCP RX path that is dropping the TURN allocate responses.
  *
  * Copyright (c) 2026.  All rights reserved.
  ******************************************************************************
  */

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lwip/opt.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/dns.h"

#include "net_diag.h"

/* ───── Raw UART helpers (direct USART register writes, bypass logging) ───── */
extern void vPetWatchdog( void );

static inline void nd_putc( char c )
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

static void nd_puts( const char *s ) { while( *s ) nd_putc( *s++ ); }

static void nd_dec( uint32_t v )
{
    char  buf[ 11 ];
    int   n = 0;
    if( v == 0 ) { nd_putc( '0' ); return; }
    while( v && n < 10 ) { buf[ n++ ] = '0' + ( v % 10 ); v /= 10; }
    while( n-- ) nd_putc( buf[ n ] );
}

static void nd_ip( const ip_addr_t *a )
{
    if( a == NULL ) { nd_puts( "(null)" ); return; }
    uint32_t raw = ip4_addr_get_u32( ip_2_ip4( a ) );
    nd_dec(   raw        & 0xFF ); nd_putc( '.' );
    nd_dec( ( raw >>  8 ) & 0xFF ); nd_putc( '.' );
    nd_dec( ( raw >> 16 ) & 0xFF ); nd_putc( '.' );
    nd_dec( ( raw >> 24 ) & 0xFF );
}

/* ───── Netmask / IP / gateway dump ─────────────────────────────────────── */
static void prvPrintConfig( struct netif *netif )
{
    nd_puts( "[NETDIAG] ======================================================\r\n" );
    nd_puts( "[NETDIAG] STA interface configuration\r\n" );

    nd_puts( "[NETDIAG]   IP      = " );
    nd_ip( netif_ip_addr4( netif ) );
    nd_puts( "\r\n" );

    nd_puts( "[NETDIAG]   Netmask = " );
    nd_ip( netif_ip_netmask4( netif ) );
    nd_puts( "\r\n" );

    nd_puts( "[NETDIAG]   Gateway = " );
    nd_ip( netif_ip_gw4( netif ) );
    nd_puts( "\r\n" );

    /* Derive the LAN network so support can see unambiguously what the
     * host believes is "LAN" vs "WAN".                                  */
    uint32_t ip   = ip4_addr_get_u32( netif_ip4_addr( netif ) );
    uint32_t mask = ip4_addr_get_u32( netif_ip4_netmask( netif ) );
    uint32_t net  = ip & mask;

    /* Count mask prefix length. */
    uint32_t hostmask = ~lwip_htonl( mask );
    int      prefix   = 32;
    while( hostmask & 1 ) { hostmask >>= 1; prefix--; }

    ip_addr_t netaddr;
    ip_addr_set_ip4_u32( &netaddr, net );
    nd_puts( "[NETDIAG]   LAN net = " );
    nd_ip( &netaddr );
    nd_puts( "/" );
    nd_dec( ( uint32_t ) prefix );
    nd_puts( "  (host-side classification boundary)\r\n" );
    nd_puts( "[NETDIAG] ======================================================\r\n" );
}

/* ───── ICMP echo probe ─────────────────────────────────────────────────── */

/* Shared state between the raw-recv callback and the probe function. */
static SemaphoreHandle_t xIcmpReplySem     = NULL;
static volatile uint16_t usExpectedId      = 0;
static volatile uint16_t usExpectedSeq     = 0;
static volatile int      lReplyReceived    = 0;
static ip_addr_t         xLastReplySrc;

static u8_t prvRawIcmpRecv( void *arg, struct raw_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr )
{
    ( void ) arg;
    ( void ) pcb;

    /* p->payload points at the IP header (lwIP delivers the full IP datagram
     * for raw ICMP PCBs).  Skip over it to reach the ICMP header. */
    struct ip_hdr *iph = ( struct ip_hdr * ) p->payload;
    if( p->len < IPH_HL( iph ) * 4 + 8 )
    {
        return 0;  /* too short, let lwIP keep it */
    }

    uint8_t *icmp_payload = ( uint8_t * ) p->payload + IPH_HL( iph ) * 4;
    uint8_t  type         = icmp_payload[ 0 ];
    uint16_t id           = ( icmp_payload[ 4 ] << 8 ) | icmp_payload[ 5 ];
    uint16_t seq          = ( icmp_payload[ 6 ] << 8 ) | icmp_payload[ 7 ];

    if( type == ICMP_ER && id == usExpectedId && seq == usExpectedSeq )
    {
        xLastReplySrc    = *addr;
        lReplyReceived   = 1;
        BaseType_t xWake = pdFALSE;
        xSemaphoreGiveFromISR( xIcmpReplySem, &xWake );
        pbuf_free( p );
        portYIELD_FROM_ISR( xWake );
        return 1; /* consumed */
    }
    return 0;     /* not ours, let lwIP keep it */
}

static int prvPingOne( const char *pcHostLabel, const ip_addr_t *pxDest )
{
    static uint16_t usIdCounter  = 0x4E44;  /* 'ND' */
    static uint16_t usSeqCounter = 1;

    nd_puts( "[NETDIAG] ping " );
    nd_puts( pcHostLabel );
    nd_puts( "  [" );
    nd_ip( pxDest );
    nd_puts( "] " );

    struct raw_pcb *pcb = raw_new( IP_PROTO_ICMP );
    if( pcb == NULL ) { nd_puts( "FAIL (raw_new)\r\n" ); return -1; }

    raw_recv( pcb, prvRawIcmpRecv, NULL );

    /* Build an ICMP echo request: 8-byte header + 32-byte payload. */
    const uint16_t usPayloadLen = 32;
    struct pbuf *p = pbuf_alloc( PBUF_IP, 8 + usPayloadLen, PBUF_RAM );
    if( p == NULL ) { raw_remove( pcb ); nd_puts( "FAIL (pbuf)\r\n" ); return -1; }

    usExpectedId    = usIdCounter;
    usExpectedSeq   = usSeqCounter++;
    lReplyReceived  = 0;

    uint8_t *buf = ( uint8_t * ) p->payload;
    memset( buf, 0, 8 + usPayloadLen );
    buf[ 0 ] = ICMP_ECHO;                     /* type 8 */
    buf[ 1 ] = 0;                             /* code   */
    buf[ 2 ] = 0; buf[ 3 ] = 0;               /* checksum (fill below) */
    buf[ 4 ] = ( uint8_t )( usExpectedId  >> 8 );
    buf[ 5 ] = ( uint8_t )  usExpectedId;
    buf[ 6 ] = ( uint8_t )( usExpectedSeq >> 8 );
    buf[ 7 ] = ( uint8_t )  usExpectedSeq;
    for( int i = 0; i < usPayloadLen; i++ ) buf[ 8 + i ] = ( uint8_t ) i;

    uint16_t chk = inet_chksum( buf, 8 + usPayloadLen );
    buf[ 2 ] = ( uint8_t )( chk        & 0xFF );
    buf[ 3 ] = ( uint8_t )( chk >>  8 );

    TickType_t xTxTick = xTaskGetTickCount();
    err_t xErr = raw_sendto( pcb, p, pxDest );
    pbuf_free( p );

    if( xErr != ERR_OK )
    {
        raw_remove( pcb );
        nd_puts( "FAIL (raw_sendto err=" );
        nd_dec( ( uint32_t )( int32_t ) xErr );
        nd_puts( ")\r\n" );
        return -1;
    }

    /* Wait up to 3 s for the reply. */
    int xGot = ( xSemaphoreTake( xIcmpReplySem, pdMS_TO_TICKS( 3000 ) ) == pdTRUE );
    raw_remove( pcb );

    if( xGot && lReplyReceived )
    {
        TickType_t xRttTicks = xTaskGetTickCount() - xTxTick;
        nd_puts( "REPLY rtt=" );
        nd_dec( ( uint32_t )( xRttTicks * portTICK_PERIOD_MS ) );
        nd_puts( "ms src=" );
        nd_ip( &xLastReplySrc );
        nd_puts( "\r\n" );
        return 0;
    }
    else
    {
        nd_puts( "TIMEOUT (no echo reply within 3000 ms)\r\n" );
        return -1;
    }
}

/* ───── Public entry point ─────────────────────────────────────────────── */
void NetDiag_Run( struct netif *netif )
{
    if( netif == NULL ) return;
    if( xIcmpReplySem == NULL )
    {
        xIcmpReplySem = xSemaphoreCreateBinary();
        if( xIcmpReplySem == NULL ) return;
    }

    prvPrintConfig( netif );

    nd_puts( "[NETDIAG] -- ICMP echo probe to WAN targets --\r\n" );
    nd_puts( "[NETDIAG] Purpose: discriminate protocol-specific UDP filter\r\n" );
    nd_puts( "[NETDIAG]          (ICMP OK / UDP dropped) from global ACL\r\n" );
    nd_puts( "[NETDIAG]          drop (ICMP also dropped).\r\n" );

    /* Known-stable ICMP responders with stable anycast IPs. */
    ip_addr_t xDst;

    IP_ADDR4( &xDst, 8, 8, 8, 8 );
    ( void ) prvPingOne( "google-dns ", &xDst );

    IP_ADDR4( &xDst, 1, 1, 1, 1 );
    ( void ) prvPingOne( "cloudflare ", &xDst );

    /* One of the AWS Kinesis Video TURN server IPs previously observed in
     * the UDP drop log.  Even if it doesn't reply to ICMP (AWS sometimes
     * rate-limits ICMP on EC2), the send path should still succeed. */
    IP_ADDR4( &xDst, 54, 172, 44, 147 );
    ( void ) prvPingOne( "aws-turn-1 ", &xDst );

    IP_ADDR4( &xDst, 13, 220, 223, 106 );
    ( void ) prvPingOne( "aws-turn-2 ", &xDst );

    nd_puts( "[NETDIAG] -- probe complete --\r\n" );
}
