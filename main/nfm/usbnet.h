#pragma once
// =====================================================================
//  usbnet  --  the USB-C port as a CDC-NCM network adapter (ncm_raw)
// ---------------------------------------------------------------------
//  usbnet_start():
//    1. drops the USB-Serial-JTAG console off the bus and hands the
//       USB-C port's full-speed PHY to the OTG 1.1 controller (the
//       launcher's and the MSC app's PHY swap, F-01). From here on the
//       console is gone (F-07): nothing may be printed until
//       usbnet_stop(), and log output is muted meanwhile;
//    2. brings up TinyUSB as a CDC-NCM device (full speed, DWC2 in slave
//       mode, D-12) in its own task on core 1.
//
//  The PC sees a USB network adapter (Linux: cdc_ncm, interface enx<MAC>).
//  The badge is 192.168.77.2, answers ARP and ping, and hands the PC
//  192.168.77.1/24 by DHCP (no route, no DNS), see netraw.h.
//
//  usbnet_stop() undoes all of it, in reverse, and the console comes
//  back as /dev/ttyACM0.
//
//  THREADING: TinyUSB and netraw are touched by the usbnet task only.
//  Other tasks hand it UDP payloads through a ring (usbnet_send_udp),
//  which any number of tasks may call.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "netraw.h"

// The badge's side of the link and the PC's.
#define USBNET_DEV_IP  {192, 168, 77, 2}
#define USBNET_HOST_IP {192, 168, 77, 1}

typedef struct {
    // USB
    bool     mounted;       // configured by the host
    uint32_t mounts;        // SET_CONFIGURATION seen
    uint32_t unmounts;      // bus reset / unplug after a mount
    // transmit (badge -> PC)
    uint32_t tx_frames;     // Ethernet frames handed to TinyUSB (data + replies)
    uint64_t tx_bytes;      // their bytes, Ethernet headers included
    uint32_t tx_udp;        // UDP payloads sent out of the ring
    uint64_t tx_udp_bytes;  // their payload bytes
    uint32_t tx_ring_full;  // usbnet_send_udp() gave up: ring full
    uint32_t tx_no_link;    // payloads dropped because the host had not configured us
    uint32_t tx_blocked;    // times TinyUSB had no NTB free (back-pressure, not loss)
    uint32_t ring_peak;     // most payloads ever waiting in the ring
    // receive (PC -> badge)
    uint32_t rx_frames;
    uint32_t rx_replies_dropped;  // a reply (ARP/ICMP/DHCP) found its queue full
    // the usbnet task's own CPU time (tud_task + ring drain), microseconds
    uint64_t task_busy_us;
    // Why the task ran (F-18/F-19): woken by an event, by its tick timeout,
    // or polled; and what TinyUSB queued, from its ISR or from a task.
    uint32_t wake_event;
    uint32_t wake_timeout;
    uint32_t wake_poll;
    uint32_t hook_isr;
    uint32_t hook_task;
    uint32_t xfer_complete;  // DCD_EVENT_XFER_COMPLETE, all endpoints
    netraw_stats_t net;
    bool           peer_seen;
} usbnet_stats_t;

// Called in the usbnet task for a UDP datagram the PC sent to the badge
// on a port netraw does not handle itself (not 7). Keep it short.
typedef void (*usbnet_udp_cb_t)(netraw_udp_t const* udp);

// MACs, from the chip's factory MAC with the locally administered bit
// set: `host` is the one the NCM descriptor announces, which Linux gives
// the PC's interface (enx<host>); `dev` is the badge's end of the link.
// Available before usbnet_start().
void usbnet_macs(uint8_t host[6], uint8_t dev[6]);

// `poll`: the task never sleeps on its semaphore but yields and runs
// again (an experiment: does the wake-up path limit the rate? F-19).
esp_err_t usbnet_start(usbnet_udp_cb_t on_udp, bool poll);
void      usbnet_stop(void);
bool      usbnet_running(void);

// Queue one UDP datagram (at most NETRAW_UDP_MAX bytes) for the PC, from
// port `port` to port `port`. Waits up to `wait` ticks for room in the
// ring; false means it was not queued (counted in tx_ring_full, or in
// tx_no_link while the host has not configured the device).
bool usbnet_send_udp(uint16_t port, void const* data, uint16_t len, TickType_t wait);

void usbnet_get_stats(usbnet_stats_t* out);
