// =====================================================================
//  tscheck  --  host tests for the host-buildable parts of nfmtest
// ---------------------------------------------------------------------
//  Build and run on the PC (no ESP headers involved):
//
//      make tscheck
//
//  Covers main/nfm/netraw.c: ARP, ICMP echo, the DHCP server's
//  DISCOVER/OFFER, REQUEST/ACK and NAK, UDP echo, the UDP header the
//  stream is sent with, and that noise (IPv6, mDNS, bad checksums) is
//  ignored rather than answered.
// =====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/nfm/netraw.h"

static int   failures = 0;
static FILE* pcap     = NULL;  // --pcap FILE: every reply, for tcpdump -r

static void pcap_open(char const* path) {
    pcap = fopen(path, "wb");
    if (!pcap) return;
    uint32_t const hdr[6] = {0xa1b2c3d4u, 0x00040002u, 0, 0, 65535, 1};  // v2.4, Ethernet
    fwrite(hdr, sizeof(hdr), 1, pcap);
}

// Every reply netraw builds goes through here.
static size_t input(netraw_t* n, uint8_t const* f, size_t flen, uint8_t* r, size_t cap, netraw_udp_t* u) {
    size_t const len = netraw_input(n, f, flen, r, cap, u);
    if (pcap && len) {
        uint32_t const rec[4] = {0, 0, (uint32_t)len, (uint32_t)len};
        fwrite(rec, sizeof(rec), 1, pcap);
        fwrite(r, len, 1, pcap);
    }
    return len;
}

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static uint8_t const DEV_MAC[6]  = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static uint8_t const HOST_MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x54};
static uint8_t const REAL_MAC[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};  // what the PC turns out to use
static uint8_t const DEV_IP[4]   = {192, 168, 77, 2};
static uint8_t const HOST_IP[4]  = {192, 168, 77, 1};
static uint8_t const BCAST[6]    = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint16_t rd16(uint8_t const* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void setup(netraw_t* n) {
    netraw_init(n, DEV_MAC, HOST_MAC, DEV_IP, HOST_IP);
}

// Ethernet + IPv4 header in front of `l4_len` bytes already at f+34.
static size_t make_ip(uint8_t* f, uint8_t const* dst_mac, uint8_t const* src_mac, uint8_t proto,
                      uint8_t const src_ip[4], uint8_t const dst_ip[4], size_t l4_len) {
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, src_mac, 6);
    wr16(f + 12, 0x0800);
    uint8_t* ip = f + 14;
    ip[0]       = 0x45;
    ip[1]       = 0;
    wr16(ip + 2, (uint16_t)(20 + l4_len));
    wr16(ip + 4, 0x1234);
    wr16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = proto;
    wr16(ip + 10, 0);
    memcpy(ip + 12, src_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    wr16(ip + 10, netraw_checksum(ip, 20));
    return 34 + l4_len;
}

static size_t make_udp(uint8_t* f, uint8_t const* dst_mac, uint8_t const src_ip[4], uint8_t const dst_ip[4],
                       uint16_t sport, uint16_t dport, uint8_t const* data, size_t len) {
    uint8_t* u = f + 34;
    wr16(u, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (uint16_t)(8 + len));
    wr16(u + 6, 0);
    memcpy(u + 8, data, len);
    return make_ip(f, dst_mac, REAL_MAC, 17, src_ip, dst_ip, 8 + len);
}

static void check_ip_reply(uint8_t const* r, size_t len, uint8_t proto) {
    CHECK(len >= 34);
    CHECK(rd16(r + 12) == 0x0800);
    CHECK(memcmp(r + 6, DEV_MAC, 6) == 0);
    CHECK(r[14] == 0x45);
    CHECK(r[14 + 9] == proto);
    CHECK(netraw_checksum(r + 14, 20) == 0);
    CHECK(rd16(r + 16) == len - 14);
}

static void test_checksum(void) {
    // RFC 1071 example: 0001 f203 f4f5 f6f7 -> sum ddf2, checksum 220d
    uint8_t const d[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(netraw_checksum(d, sizeof(d)) == 0x220d);
    uint8_t const odd[] = {0x01};
    CHECK(netraw_checksum(odd, 1) == (uint16_t)~0x0100);
}

static void test_arp(void) {
    netraw_t n;
    setup(&n);
    uint8_t f[60] = {0};
    memcpy(f, BCAST, 6);
    memcpy(f + 6, REAL_MAC, 6);
    wr16(f + 12, 0x0806);
    uint8_t* a = f + 14;
    wr16(a, 1);
    wr16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    wr16(a + 6, 1);
    memcpy(a + 8, REAL_MAC, 6);
    memcpy(a + 14, HOST_IP, 4);
    memcpy(a + 24, DEV_IP, 4);

    uint8_t r[NETRAW_FRAME_MAX];
    size_t  len = input(&n, f, sizeof(f), r, sizeof(r), NULL);
    CHECK(len == 60);
    CHECK(memcmp(r, REAL_MAC, 6) == 0);
    CHECK(rd16(r + 12) == 0x0806);
    CHECK(rd16(r + 14 + 6) == 2);
    CHECK(memcmp(r + 14 + 8, DEV_MAC, 6) == 0);
    CHECK(memcmp(r + 14 + 14, DEV_IP, 4) == 0);
    CHECK(memcmp(r + 14 + 18, REAL_MAC, 6) == 0);
    CHECK(memcmp(r + 14 + 24, HOST_IP, 4) == 0);
    CHECK(n.peer_seen && memcmp(n.peer_mac, REAL_MAC, 6) == 0);
    CHECK(n.st.rx_arp == 1);

    // For someone else: learnt from, not answered.
    a[27] = 9;
    CHECK(input(&n, f, sizeof(f), r, sizeof(r), NULL) == 0);
}

static void test_icmp(void) {
    netraw_t n;
    setup(&n);
    uint8_t f[NETRAW_FRAME_MAX] = {0};
    uint8_t* icmp               = f + 34;
    size_t const plen           = 56;  // ping's default
    icmp[0]                     = 8;
    icmp[1]                     = 0;
    wr16(icmp + 4, 0xbeef);  // id
    wr16(icmp + 6, 7);       // seq
    for (size_t i = 0; i < plen; i++) icmp[8 + i] = (uint8_t)i;
    wr16(icmp + 2, netraw_checksum(icmp, 8 + plen));
    size_t flen = make_ip(f, DEV_MAC, REAL_MAC, 1, HOST_IP, DEV_IP, 8 + plen);

    uint8_t r[NETRAW_FRAME_MAX];
    size_t  len = input(&n, f, flen, r, sizeof(r), NULL);
    CHECK(len == flen);
    check_ip_reply(r, len, 1);
    CHECK(memcmp(r, REAL_MAC, 6) == 0);
    CHECK(memcmp(r + 14 + 12, DEV_IP, 4) == 0);
    CHECK(memcmp(r + 14 + 16, HOST_IP, 4) == 0);
    uint8_t const* ri = r + 34;
    CHECK(ri[0] == 0);
    CHECK(rd16(ri + 4) == 0xbeef && rd16(ri + 6) == 7);
    CHECK(netraw_checksum(ri, 8 + plen) == 0);
    CHECK(memcmp(ri + 8, icmp + 8, plen) == 0);

    // A corrupted ICMP checksum is dropped.
    icmp[9] ^= 0xff;
    CHECK(input(&n, f, flen, r, sizeof(r), NULL) == 0);
    CHECK(n.st.rx_bad == 1);
    // So is a corrupted IP header.
    icmp[9] ^= 0xff;
    f[14 + 8] = 1;  // TTL changed, checksum not updated
    CHECK(input(&n, f, flen, r, sizeof(r), NULL) == 0);
    CHECK(n.st.rx_bad == 2);
}

// A DHCP client message: type, and optionally option 50 / 54.
static size_t make_dhcp(uint8_t* f, uint8_t type, uint8_t const* want, uint8_t const* server, uint8_t const* ciaddr) {
    uint8_t m[300] = {0};
    m[0]           = 1;
    m[1]           = 1;
    m[2]           = 6;
    m[4]           = 0xde;
    m[5]           = 0xad;
    m[6]           = 0xbe;
    m[7]           = 0xef;
    if (ciaddr) memcpy(m + 12, ciaddr, 4);
    memcpy(m + 28, REAL_MAC, 6);
    m[236] = 0x63;
    m[237] = 0x82;
    m[238] = 0x53;
    m[239] = 0x63;
    uint8_t* o = m + 240;
    *o++       = 53;
    *o++       = 1;
    *o++       = type;
    if (want) {
        *o++ = 50;
        *o++ = 4;
        memcpy(o, want, 4);
        o += 4;
    }
    if (server) {
        *o++ = 54;
        *o++ = 4;
        memcpy(o, server, 4);
        o += 4;
    }
    *o++                   = 55;  // parameter request list, as dhclient sends
    *o++                   = 3;
    *o++                   = 1;
    *o++                   = 3;
    *o++                   = 6;
    *o                     = 255;
    uint8_t const zero[4]  = {0, 0, 0, 0};
    uint8_t const bcast[4] = {255, 255, 255, 255};
    return make_udp(f, BCAST, zero, bcast, 68, 67, m, sizeof(m));
}

// Parse a DHCP reply: returns the message type, fills yiaddr.
static int dhcp_reply(uint8_t const* r, size_t len, uint8_t yiaddr[4]) {
    check_ip_reply(r, len, 17);
    CHECK(memcmp(r, BCAST, 6) == 0);
    CHECK(rd16(r + 34) == 67 && rd16(r + 36) == 68);
    CHECK(rd16(r + 38) == len - 34);
    uint8_t const* m = r + 42;
    CHECK(m[0] == 2);
    CHECK(m[4] == 0xde && m[7] == 0xef);
    CHECK(memcmp(m + 28, REAL_MAC, 6) == 0);
    memcpy(yiaddr, m + 16, 4);
    uint8_t const* o      = m + 240;
    int            type   = -1;
    int            router = 0, mask_ok = 0;
    while (*o != 255 && o < r + len) {
        if (*o == 53) type = o[2];
        if (*o == 3) router = 1;
        if (*o == 1) mask_ok = o[2] == 255 && o[3] == 255 && o[4] == 255 && o[5] == 0;
        o += 2 + o[1];
    }
    CHECK(!router);
    if (type == 2 || type == 5) CHECK(mask_ok);
    return type;
}

static void test_dhcp(void) {
    netraw_t n;
    setup(&n);
    uint8_t f[NETRAW_FRAME_MAX], r[NETRAW_FRAME_MAX], yi[4];

    size_t flen = make_dhcp(f, 1, NULL, NULL, NULL);
    size_t len  = input(&n, f, flen, r, sizeof(r), NULL);
    CHECK(len > 0);
    CHECK(dhcp_reply(r, len, yi) == 2);  // OFFER
    CHECK(memcmp(yi, HOST_IP, 4) == 0);

    flen = make_dhcp(f, 3, HOST_IP, DEV_IP, NULL);
    len  = input(&n, f, flen, r, sizeof(r), NULL);
    CHECK(dhcp_reply(r, len, yi) == 5);  // ACK
    CHECK(memcmp(yi, HOST_IP, 4) == 0);
    CHECK(n.st.dhcp_acked == 1);

    // INIT-REBOOT for an address from another network: NAK.
    uint8_t const other[4] = {10, 0, 0, 42};
    flen                   = make_dhcp(f, 3, other, NULL, NULL);
    len                    = input(&n, f, flen, r, sizeof(r), NULL);
    CHECK(dhcp_reply(r, len, yi) == 6);  // NAK

    // RENEWING: no option 50, the address in ciaddr.
    flen = make_dhcp(f, 3, NULL, NULL, HOST_IP);
    len  = input(&n, f, flen, r, sizeof(r), NULL);
    CHECK(dhcp_reply(r, len, yi) == 5);

    // A REQUEST for another server: silence.
    uint8_t const srv[4] = {192, 168, 77, 9};
    flen                 = make_dhcp(f, 3, HOST_IP, srv, NULL);
    CHECK(input(&n, f, flen, r, sizeof(r), NULL) == 0);
}

static void test_udp(void) {
    netraw_t n;
    setup(&n);
    uint8_t f[NETRAW_FRAME_MAX], r[NETRAW_FRAME_MAX];
    uint8_t const msg[] = "hello badge";

    // Echo on port 7.
    size_t flen = make_udp(f, DEV_MAC, HOST_IP, DEV_IP, 40000, 7, msg, sizeof(msg));
    size_t len  = input(&n, f, flen, r, sizeof(r), NULL);
    CHECK(len == 42 + sizeof(msg));
    check_ip_reply(r, len, 17);
    CHECK(rd16(r + 34) == 7 && rd16(r + 36) == 40000);
    CHECK(memcmp(r + 42, msg, sizeof(msg)) == 0);
    CHECK(memcmp(r, REAL_MAC, 6) == 0);  // learnt from the request

    // Another port: handed to the caller, no reply.
    netraw_udp_t u;
    flen = make_udp(f, DEV_MAC, HOST_IP, DEV_IP, 40000, 5002, msg, sizeof(msg));
    CHECK(input(&n, f, flen, r, sizeof(r), &u) == 0);
    CHECK(u.len == sizeof(msg) && u.dst_port == 5002 && u.src_port == 40000);
    CHECK(memcmp(u.data, msg, sizeof(msg)) == 0);

    // mDNS to the multicast MAC: ignored.
    uint8_t const mc_mac[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};
    uint8_t const mc_ip[4]  = {224, 0, 0, 251};
    flen                    = make_udp(f, mc_mac, HOST_IP, mc_ip, 5353, 5353, msg, sizeof(msg));
    CHECK(input(&n, f, flen, r, sizeof(r), &u) == 0);
    CHECK(u.len == 0);

    // IPv6 to our MAC: ignored.
    memcpy(f, DEV_MAC, 6);
    wr16(f + 12, 0x86dd);
    uint32_t const ign = n.st.rx_ignored;
    CHECK(input(&n, f, 80, r, sizeof(r), NULL) == 0);
    CHECK(n.st.rx_ignored == ign + 1);

    // Truncated frame.
    CHECK(input(&n, f, 10, r, sizeof(r), NULL) == 0);
}

static void test_header(void) {
    netraw_t n;
    setup(&n);
    uint8_t f[NETRAW_FRAME_MAX];
    netraw_udp_header(&n, 5000, 5000, 1316, f);
    check_ip_reply(f, 42 + 1316, 17);
    CHECK(memcmp(f, HOST_MAC, 6) == 0);
    CHECK(memcmp(f + 14 + 12, DEV_IP, 4) == 0);
    CHECK(memcmp(f + 14 + 16, HOST_IP, 4) == 0);
    CHECK(rd16(f + 34) == 5000 && rd16(f + 36) == 5000);
    CHECK(rd16(f + 38) == 8 + 1316);
    // IP ids advance, so each header gets its own checksum.
    uint16_t const id0 = rd16(f + 18);
    netraw_udp_header(&n, 5000, 5000, 1316, f);
    CHECK(rd16(f + 18) == (uint16_t)(id0 + 1));
    CHECK(netraw_checksum(f + 14, 20) == 0);
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--pcap") == 0) pcap_open(argv[2]);
    test_checksum();
    test_arp();
    test_icmp();
    test_dhcp();
    test_udp();
    test_header();
    if (pcap) fclose(pcap);
    if (failures) {
        fprintf(stderr, "tscheck: %d failure(s)\n", failures);
        return 1;
    }
    printf("tscheck: all passed\n");
    return 0;
}
