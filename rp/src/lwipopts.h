#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Common settings used in most of the pico_w examples
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

// allow override in some examples
#ifndef NO_SYS
#define NO_SYS 1
#endif

// allow override in some examples
#ifndef LWIP_SOCKET
#define LWIP_SOCKET 0
#endif
#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC 1
// Back the memp pools (PBUF_POOL etc.) with the C heap instead of a static
// .bss array. The TLS-download pbuf pool (PBUF_POOL_SIZE 28 ~= 42KB) and the
// zip inflate dict (32KB) each need a big buffer but never run at the same
// time (extraction is gated against downloads). As static pools they would
// BOTH have to be permanently reserved and 192KB of RAM can't afford that
// (heap was squeezed to ~20KB, too small for the 32KB dict). Heap-backed,
// they time-share one large heap: whichever job is active borrows the RAM.
#define MEMP_MEM_MALLOC 1
#else
// MEM_LIBC_MALLOC is incompatible with non polling versions
// Must be undefined. Setting it to 0 can break non-poll builds.
#undef MEM_LIBC_MALLOC
#endif

#define MEM_ALIGNMENT 4
#define MEM_SIZE 1024

#define MEM_SANITY_CHECK 0
#define MEM_OVERFLOW_CHECK 0

#define MEMP_NUM_PBUF 4
#define MEMP_NUM_TCP_PCB 10
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
// altcp_tls holds incoming encrypted pbufs in its rx chain until they are
// decrypted, and decryption itself allocates a PBUF_POOL pbuf. With a
// 4-entry pool the held ciphertext consumed the whole pool and decryption
// deadlocked (hardware-observed: TLS downloads timed out with zero bytes
// delivered). The pool must cover a full TCP window of held ciphertext
// plus decryption output and concurrent httpd traffic.
#define PBUF_POOL_SIZE 28
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 0
#define LWIP_RAW 0
#define TCP_MSS 1460
// TLS peers send records up to 16KB and altcp_tls only acks a record's
// bytes once the record is complete, so the receive window must exceed a
// whole record PLUS the adjacent records' overhead and lwIP's window
// update thresholds - with 12*MSS the sender stalled 335 bytes short of
// completing a 16KB record (hardware-traced zero-window probes). 20*MSS
// leaves ~12KB of margin over the worst-case record.
#define TCP_WND (20 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETCONN 0
#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
// #define ETH_PAD_SIZE                2
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_TCP_KEEPALIVE 0
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DHCP_GET_NTP_SRV 0

#ifndef NDEBUG
#define LWIP_DEBUG 1
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 1
#endif

#define ETHARP_DEBUG LWIP_DBG_OFF
#define NETIF_DEBUG LWIP_DBG_OFF
#define PBUF_DEBUG LWIP_DBG_OFF
#define API_LIB_DEBUG LWIP_DBG_OFF
#define API_MSG_DEBUG LWIP_DBG_OFF
#define SOCKETS_DEBUG LWIP_DBG_OFF
#define ICMP_DEBUG LWIP_DBG_OFF
#define INET_DEBUG LWIP_DBG_OFF
#define IP_DEBUG LWIP_DBG_OFF
#define IP_REASS_DEBUG LWIP_DBG_OFF
#define RAW_DEBUG LWIP_DBG_OFF
#define MEM_DEBUG LWIP_DBG_OFF
#define MEMP_DEBUG LWIP_DBG_OFF
#define SYS_DEBUG LWIP_DBG_OFF
#define TCP_DEBUG LWIP_DBG_OFF
#define TCP_INPUT_DEBUG LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG LWIP_DBG_OFF
#define TCP_RTO_DEBUG LWIP_DBG_OFF
#define TCP_CWND_DEBUG LWIP_DBG_OFF
#define TCP_WND_DEBUG LWIP_DBG_OFF
#define TCP_FR_DEBUG LWIP_DBG_OFF
#define TCP_QLEN_DEBUG LWIP_DBG_OFF
#define TCP_RST_DEBUG LWIP_DBG_OFF
#define UDP_DEBUG LWIP_DBG_OFF
#define TCPIP_DEBUG LWIP_DBG_OFF
#define PPP_DEBUG LWIP_DBG_OFF
#define SLIP_DEBUG LWIP_DBG_OFF
#define DHCP_DEBUG LWIP_DBG_OFF

// Custom flags

// The following is needed to test mDns
#define LWIP_MDNS_RESPONDER 1
#define LWIP_IGMP 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MDNS_RESP_USENETIF_EXTCALLBACK 1
// #define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 3)
#define MEMP_NUM_SYS_TIMEOUT (32)

// #define TCP_FAST_INTERVAL 50
#define TCP_NODELAY 1

#define LWIP_NETIF_API \
  0  //  Not needed. Sequential API, and therefore for platforms with OSes only.
#define LWIP_SOCKET \
  0  //  Not needed. Sequential API, and therefore for platforms with OSes only.

#define LWIP_TIMERS 1  // Enable timers (needed for HTTPD)
#define LWIP_HTTPD 1

#if LWIP_HTTPD == 1
#define LWIP_HTTPD_SSI 1
#define LWIP_HTTPD_CGI 1
// don't include the tag comment - less work for the CPU, but may be harder to
// debug
#define LWIP_HTTPD_SSI_INCLUDE_TAG 0
#define LWIP_HTTPD_SSI_MULTIPART 1
#define LWIP_HTTPD_DYNAMIC_HEADERS 0
#define LWIP_HTTPD_SUPPORT_POST 1
#define LWIP_HTTPD_SUPPORT_11_KEEPALIVE 1
#define LWIP_HTTPD_FILE_STATE 1
#define LWIP_HTTPD_FS_ASYNC_READ 1
#define HTTPD_POLL_INTERVAL 1
#define HTTPD_PRECALCULATED_CHECKSUM 1
#define HTTPD_USE_MEM_POOL 1
// Keep these below the working repo defaults to reduce RAM pressure with the
// per-connection JSON snapshot support used by md-browser.
#define MEMP_NUM_PARALLEL_HTTPD_CONNS 2
#define MEMP_NUM_PARALLEL_HTTPD_SSI_CONNS 2
#define LWIP_HTTPD_ABORT_ON_CLOSE_MEM_ERROR 1
#define HTTPD_FSDATA_FILE "fsdata_srv.c"
#endif

#ifndef FMANAGER_DOWNLOAD_HTTPS
#if defined(APP_DOWNLOAD_HTTPS)
#define FMANAGER_DOWNLOAD_HTTPS APP_DOWNLOAD_HTTPS
#elif defined(BOOSTER_DOWNLOAD_HTTPS)
#define FMANAGER_DOWNLOAD_HTTPS BOOSTER_DOWNLOAD_HTTPS
#else
#define FMANAGER_DOWNLOAD_HTTPS 0
#endif
#endif

#if FMANAGER_DOWNLOAD_HTTPS == 1
// If you don't want to use TLS (just a http request) you can avoid linking to
// mbedtls and remove the following
#define LWIP_ALTCP 1
#define MEMP_NUM_ALTCP_PCB 10
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1
// D-01 phase A: HTTPS is encrypted but NOT authenticated — no CA bundle
// and no peer verification, so it does not protect against an active
// MITM. Phase B (CA bundle for sidecartridge.com + VERIFY_REQUIRED)
// additionally needs a wall-clock time source for validity checks (C-02).
#define ALTCP_MBEDTLS_AUTHMODE MBEDTLS_SSL_VERIFY_NONE
// #define ALTCP_MBEDTLS_DEBUG  LWIP_DBG_ON
// #define ALTCP_MBEDTLS_LIB_DEBUG LWIP_DBG_ON
#endif

// Note bug in lwip with LWIP_ALTCP and LWIP_DEBUG
// https://savannah.nongnu.org/bugs/index.php?62159
// #define LWIP_DEBUG 1
// #undef LWIP_DEBUG
// #define LWIP_DEBUG                  1
// #define MEMP_OVERFLOW_CHECK         2
// #define MEMP_SANITY_CHECK           1

#endif /* __LWIPOPTS_H__ */
