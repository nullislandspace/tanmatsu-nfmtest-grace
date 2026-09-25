#pragma once
// =====================================================================
//  netraw  --  just enough Ethernet/IPv4 for the ncm_raw transport
// ---------------------------------------------------------------------
//  No lwIP (Part C, D-03): the badge is 192.168.77.2 on a point-to-point
//  USB link to the PC, 192.168.77.1. This module
//
//    - answers ARP for its own address,
//    - answers ICMP echo (so `ping 192.168.77.2` is the first check),
//    - runs a one-lease DHCP server that hands the PC 192.168.77.1/24
//      with no router and no DNS, so a plain NetworkManager "Wired
//      connection" configures the link by itself (no default route is
//      ever pointed at the badge),
//    - echoes UDP on port 7 (round-trip times from the PC),
//    - builds Ethernet+IPv4+UDP frames around a payload.
//
//  It is plain C with no ESP headers, so tools/tscheck.c tests it on
//  the PC with hand-made frames. It keeps no buffers of its own: the
//  caller passes the frame in and a buffer for the reply.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETRAW_ETH_HDR   14
#define NETRAW_IP_HDR    20
#define NETRAW_UDP_HDR   8
#define NETRAW_UDP_OVERHEAD (NETRAW_ETH_HDR + NETRAW_IP_HDR + NETRAW_UDP_HDR)  // 42
#define NETRAW_MTU       1500                                                  // IP packet
#define NETRAW_FRAME_MAX (NETRAW_ETH_HDR + NETRAW_MTU)                         // 1514
#define NETRAW_UDP_MAX   (NETRAW_MTU - NETRAW_IP_HDR - NETRAW_UDP_HDR)          // 1472

#define NETRAW_PORT_ECHO 7

typedef struct {
    uint32_t rx_frames;   // every frame handed in
    uint32_t rx_arp;      // ARP requests for us
    uint32_t rx_icmp;     // echo requests for us
    uint32_t rx_dhcp;     // DHCP DISCOVER/REQUEST
    uint32_t rx_udp;      // other UDP for us (echo included)
    uint32_t rx_ignored;  // not for us, or a protocol we do not speak (IPv6, mDNS, ...)
    uint32_t rx_bad;      // truncated, bad checksum, fragments
    uint32_t tx_replies;  // replies built (ARP + ICMP + DHCP + echo)
    uint32_t dhcp_acked;  // leases handed out
} netraw_stats_t;

typedef struct {
    uint8_t  own_mac[6];
    uint8_t  own_ip[4];
    uint8_t  peer_mac[6];  // the PC's interface; learnt from its traffic
    uint8_t  peer_ip[4];
    uint8_t  mask[4];
    bool     peer_seen;    // peer_mac came from the wire, not from the start value
    uint16_t ip_id;
    netraw_stats_t st;
} netraw_t;

// A UDP datagram for us that netraw does not answer itself.
typedef struct {
    uint8_t const* data;
    uint16_t       len;
    uint16_t       src_port;
    uint16_t       dst_port;
} netraw_udp_t;

// `peer_mac` is where to send before anything has been heard from the
// PC: the host-side MAC the NCM descriptor announced, which Linux gives
// the interface. Addresses are the 192.168.77.x pair above.
void netraw_init(netraw_t* n, uint8_t const own_mac[6], uint8_t const peer_mac[6], uint8_t const own_ip[4],
                 uint8_t const peer_ip[4]);

// Handle one received Ethernet frame.
//   - Returns the length of a reply frame written to `reply` (at most
//     `cap` bytes; NETRAW_FRAME_MAX always suffices), or 0 for none.
//   - A UDP datagram for us that it does not answer itself is described
//     in `*udp` (which points into `frame`), if `udp` is not NULL;
//     otherwise udp->len is set to 0.
size_t netraw_input(netraw_t* n, uint8_t const* frame, size_t len, uint8_t* reply, size_t cap, netraw_udp_t* udp);

// Write the 42-byte Ethernet+IPv4+UDP header for a datagram of
// `payload_len` bytes to the peer into `out`. The payload goes right
// after it (out + NETRAW_UDP_OVERHEAD). The UDP checksum is left 0,
// which IPv4 allows; the IPv4 header checksum is filled in.
void netraw_udp_header(netraw_t* n, uint16_t src_port, uint16_t dst_port, uint16_t payload_len, uint8_t* out);

// The Internet checksum (RFC 1071) over `len` bytes: the value to store
// in a header (host order; store it big-endian). Over a header that
// already holds a correct checksum it returns 0.
uint16_t netraw_checksum(uint8_t const* data, size_t len);
