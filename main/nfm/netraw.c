// =====================================================================
//  netraw  --  Ethernet/ARP/IPv4/ICMP/UDP/DHCP for ncm_raw (see netraw.h)
// =====================================================================

#include "netraw.h"
#include <string.h>

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IPPROTO_ICMP_ 1
#define IPPROTO_UDP_  17

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u
#define DHCP_LEASE_S     86400u

// BOOTP fixed part: op .. file, then the 4-byte magic cookie.
#define BOOTP_FIXED   236
#define BOOTP_MIN_LEN 300  // what BOOTP relays and old clients expect

enum { DHCP_DISCOVER = 1, DHCP_OFFER = 2, DHCP_REQUEST = 3, DHCP_ACK = 5, DHCP_NAK = 6 };

static uint8_t const BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static uint8_t const BROADCAST_IP[4]  = {255, 255, 255, 255};

// --- byte order helpers (the wire is big-endian) ----------------------

static inline uint16_t rd16(uint8_t const* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(uint8_t const* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint16_t netraw_checksum(uint8_t const* data, size_t len) {
    uint32_t sum = 0;
    size_t   i   = 0;
    for (; i + 1 < len; i += 2) sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (i < len) sum += (uint32_t)(data[i] << 8);
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

void netraw_init(netraw_t* n, uint8_t const own_mac[6], uint8_t const peer_mac[6], uint8_t const own_ip[4],
                 uint8_t const peer_ip[4]) {
    memset(n, 0, sizeof(*n));
    memcpy(n->own_mac, own_mac, 6);
    memcpy(n->peer_mac, peer_mac, 6);
    memcpy(n->own_ip, own_ip, 4);
    memcpy(n->peer_ip, peer_ip, 4);
    n->mask[0] = n->mask[1] = n->mask[2] = 255;
    n->mask[3] = 0;
}

// --- frame builders ----------------------------------------------------

static void eth_header(uint8_t* out, uint8_t const dst[6], uint8_t const src[6], uint16_t type) {
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    wr16(out + 12, type);
}

static void ip_header(netraw_t* n, uint8_t* ip, uint8_t proto, uint8_t const src[4], uint8_t const dst[4],
                      uint16_t total_len) {
    ip[0] = 0x45;  // v4, 20-byte header
    ip[1] = 0;     // DSCP/ECN
    wr16(ip + 2, total_len);
    wr16(ip + 4, n->ip_id++);
    wr16(ip + 6, 0x4000);  // don't fragment
    ip[8] = 64;            // TTL
    ip[9] = proto;
    wr16(ip + 10, 0);
    memcpy(ip + 12, src, 4);
    memcpy(ip + 16, dst, 4);
    wr16(ip + 10, netraw_checksum(ip, NETRAW_IP_HDR));
}

// Ethernet + IPv4 + UDP header for `payload_len` bytes, any destination.
static void udp_header_to(netraw_t* n, uint8_t* out, uint8_t const dst_mac[6], uint8_t const dst_ip[4],
                          uint16_t src_port, uint16_t dst_port, uint16_t payload_len) {
    eth_header(out, dst_mac, n->own_mac, ETHERTYPE_IPV4);
    uint8_t* ip = out + NETRAW_ETH_HDR;
    ip_header(n, ip, IPPROTO_UDP_, n->own_ip, dst_ip, (uint16_t)(NETRAW_IP_HDR + NETRAW_UDP_HDR + payload_len));
    uint8_t* udp = ip + NETRAW_IP_HDR;
    wr16(udp + 0, src_port);
    wr16(udp + 2, dst_port);
    wr16(udp + 4, (uint16_t)(NETRAW_UDP_HDR + payload_len));
    wr16(udp + 6, 0);  // no checksum (allowed for IPv4)
}

void netraw_udp_header(netraw_t* n, uint16_t src_port, uint16_t dst_port, uint16_t payload_len, uint8_t* out) {
    udp_header_to(n, out, n->peer_mac, n->peer_ip, src_port, dst_port, payload_len);
}

// --- ARP -------------------------------------------------------------------

static size_t arp_input(netraw_t* n, uint8_t const* arp, size_t len, uint8_t* reply, size_t cap) {
    if (len < 28) {
        n->st.rx_bad++;
        return 0;
    }
    // Ethernet/IPv4 only.
    if (rd16(arp) != 1 || rd16(arp + 2) != ETHERTYPE_IPV4 || arp[4] != 6 || arp[5] != 4) {
        n->st.rx_ignored++;
        return 0;
    }
    uint16_t const       op  = rd16(arp + 6);
    uint8_t const* const sha = arp + 8;
    uint8_t const* const spa = arp + 14;
    uint8_t const* const tpa = arp + 24;

    // Whatever the PC says about itself, believe it: that is its MAC.
    if (memcmp(spa, n->peer_ip, 4) == 0) {
        memcpy(n->peer_mac, sha, 6);
        n->peer_seen = true;
    }
    if (op != 1 || memcmp(tpa, n->own_ip, 4) != 0) {
        n->st.rx_ignored++;
        return 0;
    }
    n->st.rx_arp++;
    if (cap < NETRAW_ETH_HDR + 28) return 0;

    eth_header(reply, sha, n->own_mac, ETHERTYPE_ARP);
    uint8_t* r = reply + NETRAW_ETH_HDR;
    wr16(r + 0, 1);
    wr16(r + 2, ETHERTYPE_IPV4);
    r[4] = 6;
    r[5] = 4;
    wr16(r + 6, 2);  // reply
    memcpy(r + 8, n->own_mac, 6);
    memcpy(r + 14, n->own_ip, 4);
    memcpy(r + 18, sha, 6);
    memcpy(r + 24, spa, 4);
    n->st.tx_replies++;
    // Pad to the 60-byte Ethernet minimum; the host's NIC would, USB does not.
    memset(reply + NETRAW_ETH_HDR + 28, 0, 60 - (NETRAW_ETH_HDR + 28));
    return 60;
}

// --- ICMP --------------------------------------------------------------------

static size_t icmp_input(netraw_t* n, uint8_t const* ip, size_t ip_len, size_t hl, uint8_t* reply, size_t cap) {
    uint8_t const* const icmp     = ip + hl;
    size_t const         icmp_len = ip_len - hl;
    if (icmp_len < 8) {
        n->st.rx_bad++;
        return 0;
    }
    if (icmp[0] != 8) {  // echo request only
        n->st.rx_ignored++;
        return 0;
    }
    if (netraw_checksum(icmp, icmp_len) != 0) {
        n->st.rx_bad++;
        return 0;
    }
    n->st.rx_icmp++;
    size_t const total = NETRAW_ETH_HDR + NETRAW_IP_HDR + icmp_len;
    if (total > cap) return 0;

    eth_header(reply, n->peer_mac, n->own_mac, ETHERTYPE_IPV4);
    uint8_t* rip = reply + NETRAW_ETH_HDR;
    ip_header(n, rip, IPPROTO_ICMP_, n->own_ip, ip + 12, (uint16_t)(NETRAW_IP_HDR + icmp_len));
    uint8_t* ricmp = rip + NETRAW_IP_HDR;
    memcpy(ricmp, icmp, icmp_len);
    ricmp[0] = 0;  // echo reply
    wr16(ricmp + 2, 0);
    wr16(ricmp + 2, netraw_checksum(ricmp, icmp_len));
    n->st.tx_replies++;
    return total;
}

// --- DHCP server (one lease: the peer address) ----------------------------------

// Find option `code` in the options area; its length in *olen.
static uint8_t const* dhcp_option(uint8_t const* opt, size_t len, uint8_t code, uint8_t* olen) {
    size_t i = 0;
    while (i < len) {
        uint8_t const c = opt[i];
        if (c == 0) {  // pad
            i++;
            continue;
        }
        if (c == 255 || i + 1 >= len) break;  // end
        uint8_t const l = opt[i + 1];
        if (i + 2 + l > len) break;
        if (c == code) {
            *olen = l;
            return opt + i + 2;
        }
        i += 2u + l;
    }
    return NULL;
}

static size_t dhcp_input(netraw_t* n, uint8_t const* msg, size_t len, uint8_t* reply, size_t cap) {
    if (len < BOOTP_FIXED + 4 || msg[0] != 1 || msg[1] != 1 || msg[2] != 6 || rd32(msg + BOOTP_FIXED) != DHCP_MAGIC) {
        n->st.rx_bad++;
        return 0;
    }
    uint8_t const* const opts  = msg + BOOTP_FIXED + 4;
    size_t const         olen_ = len - (BOOTP_FIXED + 4);
    uint8_t              l     = 0;
    uint8_t const*       type  = dhcp_option(opts, olen_, 53, &l);
    if (type == NULL || l != 1) {
        n->st.rx_bad++;
        return 0;
    }

    uint8_t answer;
    if (type[0] == DHCP_DISCOVER) {
        answer = DHCP_OFFER;
    } else if (type[0] == DHCP_REQUEST) {
        // A REQUEST naming another server is that server's business.
        uint8_t const* sid = dhcp_option(opts, olen_, 54, &l);
        if (sid != NULL && (l != 4 || memcmp(sid, n->own_ip, 4) != 0)) {
            n->st.rx_ignored++;
            return 0;
        }
        // The address asked for: option 50 (SELECTING/INIT-REBOOT) or
        // ciaddr (RENEWING/REBINDING). Anything but the peer address gets
        // a NAK, so the client starts over with a DISCOVER.
        uint8_t const* want = dhcp_option(opts, olen_, 50, &l);
        if (want == NULL || l != 4) want = msg + 12;
        answer = memcmp(want, n->peer_ip, 4) == 0 ? DHCP_ACK : DHCP_NAK;
    } else {
        n->st.rx_ignored++;  // RELEASE, DECLINE, INFORM: nothing to do
        return 0;
    }
    n->st.rx_dhcp++;

    size_t const total = NETRAW_UDP_OVERHEAD + BOOTP_MIN_LEN;
    if (total > cap) return 0;
    uint8_t* b = reply + NETRAW_UDP_OVERHEAD;
    memset(b, 0, BOOTP_MIN_LEN);
    b[0] = 2;  // BOOTREPLY
    b[1] = 1;
    b[2] = 6;
    memcpy(b + 4, msg + 4, 4);    // xid
    memcpy(b + 10, msg + 10, 2);  // flags
    if (answer != DHCP_NAK) {
        memcpy(b + 16, n->peer_ip, 4);  // yiaddr
        memcpy(b + 20, n->own_ip, 4);   // siaddr
    }
    memcpy(b + 28, msg + 28, 16);  // chaddr
    wr32(b + BOOTP_FIXED, DHCP_MAGIC);
    uint8_t* o = b + BOOTP_FIXED + 4;
    *o++       = 53;
    *o++       = 1;
    *o++       = answer;
    *o++       = 54;
    *o++       = 4;
    memcpy(o, n->own_ip, 4);
    o += 4;
    if (answer != DHCP_NAK) {
        *o++ = 51;  // lease time
        *o++ = 4;
        wr32(o, DHCP_LEASE_S);
        o += 4;
        *o++ = 1;  // subnet mask; deliberately no router (3) and no DNS (6)
        *o++ = 4;
        memcpy(o, n->mask, 4);
        o += 4;
    }
    *o = 255;

    // Broadcast: the client has no address yet (RFC 2131 4.1).
    udp_header_to(n, reply, BROADCAST_MAC, BROADCAST_IP, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, BOOTP_MIN_LEN);
    n->st.tx_replies++;
    if (answer == DHCP_ACK) n->st.dhcp_acked++;
    return total;
}

// --- UDP ---------------------------------------------------------------------

static size_t udp_input(netraw_t* n, uint8_t const* ip, size_t ip_len, size_t hl, bool for_us, uint8_t* reply,
                        size_t cap, netraw_udp_t* out) {
    uint8_t const* const udp = ip + hl;
    if (ip_len - hl < NETRAW_UDP_HDR) {
        n->st.rx_bad++;
        return 0;
    }
    uint16_t const sport = rd16(udp);
    uint16_t const dport = rd16(udp + 2);
    uint16_t const ulen  = rd16(udp + 4);
    if (ulen < NETRAW_UDP_HDR || ulen > ip_len - hl) {
        n->st.rx_bad++;
        return 0;
    }
    uint8_t const* const data = udp + NETRAW_UDP_HDR;
    uint16_t const       dlen = (uint16_t)(ulen - NETRAW_UDP_HDR);

    // DHCP arrives broadcast from 0.0.0.0.
    if (dport == DHCP_SERVER_PORT && sport == DHCP_CLIENT_PORT) return dhcp_input(n, data, dlen, reply, cap);
    if (!for_us) {
        n->st.rx_ignored++;  // mDNS, LLMNR, NetBIOS, SSDP: the PC's chatter
        return 0;
    }
    n->st.rx_udp++;
    if (dport == NETRAW_PORT_ECHO) {
        size_t const total = NETRAW_UDP_OVERHEAD + dlen;
        if (total > cap) return 0;
        udp_header_to(n, reply, n->peer_mac, ip + 12, NETRAW_PORT_ECHO, sport, dlen);
        memcpy(reply + NETRAW_UDP_OVERHEAD, data, dlen);
        n->st.tx_replies++;
        return total;
    }
    if (out != NULL) {
        out->data     = data;
        out->len      = dlen;
        out->src_port = sport;
        out->dst_port = dport;
    }
    return 0;
}

// --- IPv4 --------------------------------------------------------------------

static size_t ipv4_input(netraw_t* n, uint8_t const* ip, size_t len, uint8_t const* src_mac, uint8_t* reply,
                         size_t cap, netraw_udp_t* udp) {
    if (len < NETRAW_IP_HDR || (ip[0] >> 4) != 4) {
        n->st.rx_bad++;
        return 0;
    }
    size_t const   hl    = (size_t)(ip[0] & 0x0f) * 4;
    uint16_t const total = rd16(ip + 2);
    if (hl < NETRAW_IP_HDR || total < hl || total > len || netraw_checksum(ip, hl) != 0) {
        n->st.rx_bad++;
        return 0;
    }
    // Fragments are never needed on this link; drop them rather than
    // pretend to reassemble.
    if ((rd16(ip + 6) & 0x3fff) != 0) {
        n->st.rx_bad++;
        return 0;
    }
    uint8_t const* const dst = ip + 16;
    if (memcmp(ip + 12, n->peer_ip, 4) == 0) {
        memcpy(n->peer_mac, src_mac, 6);
        n->peer_seen = true;
    }
    bool const unicast = memcmp(dst, n->own_ip, 4) == 0;

    if (ip[9] == IPPROTO_UDP_) return udp_input(n, ip, total, hl, unicast, reply, cap, udp);
    if (ip[9] == IPPROTO_ICMP_ && unicast) return icmp_input(n, ip, total, hl, reply, cap);
    n->st.rx_ignored++;
    return 0;
}

size_t netraw_input(netraw_t* n, uint8_t const* frame, size_t len, uint8_t* reply, size_t cap, netraw_udp_t* udp) {
    if (udp != NULL) udp->len = 0;
    n->st.rx_frames++;
    if (len < NETRAW_ETH_HDR) {
        n->st.rx_bad++;
        return 0;
    }
    uint8_t const* const dst = frame;
    if (memcmp(dst, n->own_mac, 6) != 0 && memcmp(dst, BROADCAST_MAC, 6) != 0) {
        n->st.rx_ignored++;  // multicast (IPv6 ND, mDNS) and anything else
        return 0;
    }
    uint16_t const type = rd16(frame + 12);
    if (type == ETHERTYPE_ARP) return arp_input(n, frame + NETRAW_ETH_HDR, len - NETRAW_ETH_HDR, reply, cap);
    if (type == ETHERTYPE_IPV4)
        return ipv4_input(n, frame + NETRAW_ETH_HDR, len - NETRAW_ETH_HDR, frame + 6, reply, cap, udp);
    n->st.rx_ignored++;
    return 0;
}
