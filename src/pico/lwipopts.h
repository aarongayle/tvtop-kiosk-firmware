// lwIP configuration for the kiosk (pico_cyw43_arch_lwip_threadsafe_background).
//
// Derived from pico-examples/pico_w/wifi/lwipopts_examples_common.h, then sized for a device
// whose RAM is mostly spent on the line pool. Two profiles, chosen by KIOSK_TLS (set by CMake):
//
//   KIOSK_TLS=0 (plain HTTP, local server)              KIOSK_TLS=1 (mbedTLS)
//   MEM_SIZE        4000  (~4 KB heap)                  6000
//   PBUF_POOL_SIZE     8  (8 × ~1.5 KB = ~12.5 KB)     24  (~37 KB)
//   TCP_WND      4 × MSS  (5840)                        32768 (see comment below)
//   TCP_SND_BUF  2 × MSS  (2920)                        2 × MSS
//   MEMP_NUM_TCP_SEG  16  (16 × ~24 B)                  16
//   total lwIP RAM   ~24 KB                             ~64 KB incl. mbedTLS's 16 KB record buffer
//
// The HTTP client never copies a body: received pbufs stay queued (window shrinks) until the main
// loop has decoded them, so TCP_WND is the only buffering a 320 KB static set streams through.
#ifndef KIOSK_LWIPOPTS_H
#define KIOSK_LWIPOPTS_H

#ifndef KIOSK_TLS
#define KIOSK_TLS 0
#endif

// ---- system model ------------------------------------------------------------------------
#define NO_SYS                      1     // raw API only; callbacks run in the cyw43 background context
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0     // incompatible with the background (IRQ) arch: lwIP heap is a static MEM_SIZE array
#define MEM_ALIGNMENT               4

// ---- protocols ---------------------------------------------------------------------------
#define LWIP_IPV4                   1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1     // cyw43 driver wants it
#define LWIP_UDP                    1     // DHCP, DNS, and the provisioning DHCP/DNS servers
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2     // primary + secondary from DHCP; each entry is one ip_addr_t
#define DNS_TABLE_SIZE              2     // we only ever resolve the kiosk host (2 × ~40 B)
#define LWIP_TCP_KEEPALIVE          1     // detect a dead long-poll peer behind NAT without waiting for our own timeout
#define LWIP_NETIF_HOSTNAME         1     // "tvtop-kiosk" in DHCP so the router UI shows a name
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_TX_SINGLE_PBUF   1     // the cyw43 driver sends one contiguous buffer per frame
#define DHCP_DOES_ARP_CHECK         0     // saves ~1 s at boot; collisions are the router's problem
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_CHKSUM_ALGORITHM       3
#define MEMP_NUM_ARP_QUEUE          4     // packets queued while resolving the gateway's MAC (4 × ~16 B)

// ---- memory: profile by KIOSK_TLS --------------------------------------------------------
#define TCP_MSS                     1460
#if KIOSK_TLS
// mbedTLS needs the whole encrypted record before it can decrypt any of it, and altcp only
// acknowledges application bytes after we consume them. A 16384-byte plaintext record is ~16413
// raw bytes; with TCP_WND == 16384 the server fills the window before the record is complete,
// altcp_recved is never called and the connection deadlocks (pico-examples tls_client/lwipopts.h
// documents the same). 2× lets one record be in flight while the previous one is drained.
#define TCP_WND                     32768
#define MEM_SIZE                    6000  // lwIP heap: TCP segments' pbufs on the send path + altcp_tls pcb wrappers
#define PBUF_POOL_SIZE              24    // RX pool must cover TCP_WND: 24 × PBUF_POOL_BUFSIZE (~1.5 KB) ≈ 37 KB
#define MEMP_NUM_TCP_SEG            16
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
#else
#define TCP_WND                     (4 * TCP_MSS)   // 5840 B receive window: 4 full segments in flight is enough for a 20 Mbit link at 1 ms RTT
#define MEM_SIZE                    4000            // lwIP heap (static array): TCP send segments, DHCP/DNS transient allocations
#define PBUF_POOL_SIZE              8               // 8 × ~1.5 KB ≈ 12.5 KB of RX pbufs; must cover TCP_WND (5840) plus a few for ARP/DHCP
#define MEMP_NUM_TCP_SEG            16              // queued TX segments (16 × ~24 B); ≥ TCP_SND_QUEUELEN (8)
#define LWIP_ALTCP                  1               // the HTTP client speaks altcp in both profiles; plain TCP via altcp_tcp
#define LWIP_ALTCP_TLS              0
#endif
#define TCP_SND_BUF                 (2 * TCP_MSS)   // our requests are < 1 KB; the minimum lwIP's sanity check allows
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define MEMP_NUM_TCP_PCB            4     // 1 kiosk connection + provisioning HTTP server + slack; 4 × ~160 B
#define MEMP_NUM_TCP_PCB_LISTEN     2     // provisioning portal listener
#define MEMP_NUM_UDP_PCB            4     // DHCP client, DNS client, DHCP server, DNS server (portal)
#define MEMP_NUM_PBUF               8     // PBUF_REF/ROM pbufs (we write with TCP_WRITE_FLAG_COPY so few are needed)

// ---- diagnostics: none in release --------------------------------------------------------
#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#else
#define LWIP_STATS                  0
#endif
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

#define ETHARP_DEBUG                LWIP_DBG_OFF
#define NETIF_DEBUG                 LWIP_DBG_OFF
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define API_LIB_DEBUG               LWIP_DBG_OFF
#define API_MSG_DEBUG               LWIP_DBG_OFF
#define SOCKETS_DEBUG               LWIP_DBG_OFF
#define ICMP_DEBUG                  LWIP_DBG_OFF
#define INET_DEBUG                  LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF
#define IP_REASS_DEBUG              LWIP_DBG_OFF
#define RAW_DEBUG                   LWIP_DBG_OFF
#define MEM_DEBUG                   LWIP_DBG_OFF
#define MEMP_DEBUG                  LWIP_DBG_OFF
#define SYS_DEBUG                   LWIP_DBG_OFF
#define TCP_DEBUG                   LWIP_DBG_OFF
#define TCP_INPUT_DEBUG             LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG            LWIP_DBG_OFF
#define TCP_RTO_DEBUG               LWIP_DBG_OFF
#define TCP_CWND_DEBUG              LWIP_DBG_OFF
#define TCP_WND_DEBUG               LWIP_DBG_OFF
#define TCP_FR_DEBUG                LWIP_DBG_OFF
#define TCP_QLEN_DEBUG              LWIP_DBG_OFF
#define TCP_RST_DEBUG               LWIP_DBG_OFF
#define UDP_DEBUG                   LWIP_DBG_OFF
#define TCPIP_DEBUG                 LWIP_DBG_OFF
#define PPP_DEBUG                   LWIP_DBG_OFF
#define SLIP_DEBUG                  LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_OFF
#define DNS_DEBUG                   LWIP_DBG_OFF
#define ALTCP_MBEDTLS_DEBUG         LWIP_DBG_OFF

#endif // KIOSK_LWIPOPTS_H
