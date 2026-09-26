// =====================================================================
//  usbnet  --  the USB-C port as a CDC-NCM network adapter (see usbnet.h)
// =====================================================================

#include "usbnet.h"
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "hal/efuse_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "device/dcd.h"
#include "tusb.h"

// --- configuration -----------------------------------------------------------

// USB IDs: a pid.codes test PID while this is a test app (D-10, F-12).
// Linux binds cdc_ncm by interface class, so the IDs do not matter to it;
// what matters is that they are NOT the launcher's 0x16D0:0x0F9A, which
// BadgeLink tools would try to talk to.
#define USB_VID 0x1209
#define USB_PID 0x0001

#define TASK_STACK    6144
#define TASK_PRIO     10  // above the stream tasks (Part B); it only moves bytes
#define TASK_CORE     1
#define WAKE_TICKS    1  // at most one tick (10 ms) between drains when nothing wakes us
#define START_TIMEOUT pdMS_TO_TICKS(2000)

#define RING_N  64  // UDP payloads waiting for TinyUSB (PSRAM, ~94 KB)
#define REPLY_N 4   // ARP/ICMP/DHCP/echo replies waiting (PSRAM, ~6 KB)

enum { KIND_REPLY = 1, KIND_UDP = 2 };

// Endpoints. OTG 1.1 has 7 endpoints, 5 of them IN-capable (dwc2_esp32.h).
#define EP_NOTIF 0x81
#define EP_OUT   0x02
#define EP_IN    0x82
#define EP_SIZE  64  // full-speed bulk

enum { ITF_NUM_NCM = 0, ITF_NUM_NCM_DATA, ITF_COUNT };
enum { STR_LANG = 0, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_INTERFACE, STR_MAC, STR_COUNT };

// --- state -------------------------------------------------------------------

typedef struct {
    uint16_t port;
    uint16_t len;
    uint8_t  data[NETRAW_UDP_MAX];
} slot_t;

typedef struct {
    uint16_t len;
    uint8_t  frame[NETRAW_FRAME_MAX];
} reply_t;

static netraw_t          s_net;
static usbnet_stats_t    s_st;
static usbnet_udp_cb_t   s_on_udp;
static usb_phy_handle_t  s_phy;
static TaskHandle_t      s_task;
static SemaphoreHandle_t s_wake;      // anything for the task to do
static SemaphoreHandle_t s_space;     // the task freed ring slots
static SemaphoreHandle_t s_done;      // the task finished starting / stopping
static SemaphoreHandle_t s_prod;      // producers' mutex (ring head, their counters)
static volatile bool     s_stop;
static volatile bool     s_init_ok;
static volatile bool     s_running;
static volatile bool     s_mounted;
static bool              s_poll;

// Ring of UDP payloads: producers advance head under s_prod, the task
// advances tail. Indices run freely; the slot is index % RING_N.
static slot_t*  s_ring;
static uint32_t s_head;
static uint32_t s_tail;

// Replies are produced and consumed by the task alone.
static reply_t* s_replies;
static uint32_t s_reply_head;
static uint32_t s_reply_tail;

static uint8_t s_host_mac[6];
static uint8_t s_dev_mac[6];
static char    s_mac_str[13];

// --- MACs ----------------------------------------------------------------------

void usbnet_macs(uint8_t host[6], uint8_t dev[6]) {
    // The factory MAC, in the byte order esp_efuse_mac_get_default() uses
    // (not exported by graceloader; the eFuse registers are).
    uint32_t const m0      = efuse_ll_get_mac0();
    uint32_t const m1      = efuse_ll_get_mac1();
    uint8_t        base[6] = {(uint8_t)(m1 >> 8), (uint8_t)m1,         (uint8_t)(m0 >> 24),
                              (uint8_t)(m0 >> 16), (uint8_t)(m0 >> 8), (uint8_t)m0};
    // Locally administered, unicast; one MAC per end of the link.
    base[0] = (uint8_t)((base[0] | 0x02) & ~0x01);
    memcpy(host, base, 6);
    memcpy(dev, base, 6);
    dev[5] ^= 0x01;
}

// --- PHY swap (after tanmatsu-launcher usb_device.c, tanmatsu-usb-msc) -------------

static void phy_to_otg(void) {
    usb_serial_jtag_pull_override_vals_t const off = {.dm_pd = true, .dm_pu = false, .dp_pd = true, .dp_pu = false};
    usb_serial_jtag_pull_override_vals_t const on  = {.dm_pd = false, .dm_pu = false, .dp_pd = false, .dp_pu = true};
    // Drop the console off the bus, give the host time to see it go.
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    vTaskDelay(pdMS_TO_TICKS(500));
    // USJ -> FSLS PHY 1 (no pads), USB_WRAP/OTG 1.1 -> FSLS PHY 0 (the USB-C port).
    usb_serial_jtag_ll_phy_select(1);
    usb_serial_jtag_ll_phy_enable_pull_override(&on);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

static void phy_to_usj(void) {
    usb_serial_jtag_pull_override_vals_t const off = {.dm_pd = true, .dm_pu = false, .dp_pd = true, .dp_pu = false};
    usb_serial_jtag_pull_override_vals_t const on  = {.dm_pd = false, .dm_pu = false, .dp_pd = false, .dp_pu = true};
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    usb_serial_jtag_ll_phy_select(0);
    vTaskDelay(pdMS_TO_TICKS(500));
    // Back on the bus: the host enumerates the console again.
    usb_serial_jtag_ll_phy_enable_pull_override(&on);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

// --- descriptors ---------------------------------------------------------------

static tusb_desc_device_t const s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // IAD in the configuration: Misc / Common / IAD at device level.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)

static uint8_t const s_desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NCM, STR_INTERFACE, STR_MAC, EP_NOTIF, 64, EP_OUT, EP_IN, EP_SIZE,
                           CFG_TUD_NET_MTU),
};

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&s_desc_device;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_desc_config;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[1 + 32];
    char const*     str;
    switch (index) {
        case STR_LANG:
            desc[1] = 0x0409;
            desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
            return desc;
        case STR_MANUFACTURER: str = "Tanmatsu"; break;
        case STR_PRODUCT: str = "Tanmatsu nfmtest"; break;
        case STR_SERIAL: str = s_mac_str; break;
        case STR_INTERFACE: str = "nfmtest NCM"; break;
        case STR_MAC: str = s_mac_str; break;  // the PC's interface MAC, 12 hex digits
        default: return NULL;
    }
    size_t n = strlen(str);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) desc[1 + i] = (uint8_t)str[i];
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc;
}

// --- TinyUSB callbacks (all in the usbnet task, except the event hook) ------------

void tud_mount_cb(void) {
    s_mounted = true;
    s_st.mounts++;
}

void tud_umount_cb(void) {
    s_mounted = false;
    s_st.unmounts++;
}

// Every event TinyUSB queues, from its ISR or from a task: wake the
// usbnet task, which then runs tud_task. This is also how it learns that
// an IN transfer finished and NTBs are free again.
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    if (s_wake == NULL) return;
    if (eventid == DCD_EVENT_XFER_COMPLETE) s_st.xfer_complete++;
    if (in_isr) {
        s_st.hook_isr++;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_wake, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        s_st.hook_task++;
        xSemaphoreGive(s_wake);
    }
}

// Only the ECM/RNDIS driver uses these two; defined so nothing dangles.
uint8_t tud_network_mac_address[6];

void tud_network_init_cb(void) {}

bool tud_network_recv_cb(uint8_t const* src, uint16_t size) {
    s_st.rx_frames++;
    static uint8_t scratch[NETRAW_FRAME_MAX];  // a reply with nowhere to go
    bool const     room = s_reply_head - s_reply_tail < REPLY_N;
    reply_t* const r    = room ? &s_replies[s_reply_head % REPLY_N] : NULL;
    netraw_udp_t   udp;
    size_t const   n = netraw_input(&s_net, src, size, r ? r->frame : scratch, NETRAW_FRAME_MAX, &udp);
    if (n > 0) {
        if (r) {
            r->len = (uint16_t)n;
            s_reply_head++;
        } else {
            s_st.rx_replies_dropped++;
        }
    }
    if (udp.len > 0 && s_on_udp != NULL) s_on_udp(&udp);
    // Done with the datagram: let TinyUSB hand over the next one.
    tud_network_recv_renew();
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg) {
    if (arg == KIND_REPLY) {
        reply_t const* r = ref;
        memcpy(dst, r->frame, r->len);
        return r->len;
    }
    slot_t const* s = ref;
    netraw_udp_header(&s_net, s->port, s->port, s->len, dst);
    memcpy(dst + NETRAW_UDP_OVERHEAD, s->data, s->len);
    return (uint16_t)(NETRAW_UDP_OVERHEAD + s->len);
}

// --- the task ----------------------------------------------------------------------

static void drain(void) {
    // Replies first: they are few, small, and someone is waiting for them.
    while (s_reply_head != s_reply_tail) {
        reply_t* r = &s_replies[s_reply_tail % REPLY_N];
        if (!tud_network_can_xmit(r->len)) {
            s_st.tx_blocked++;
            return;
        }
        tud_network_xmit(r, KIND_REPLY);
        s_reply_tail++;
        s_st.tx_frames++;
        s_st.tx_bytes += r->len;
    }

    uint32_t const head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint32_t       tail = s_tail;
    if (head == tail) return;

    if (!tud_mounted()) {
        // Nobody to send to: drop rather than let producers block.
        s_st.tx_no_link += head - tail;
        __atomic_store_n(&s_tail, head, __ATOMIC_RELEASE);
        xSemaphoreGive(s_space);
        return;
    }

    bool freed = false;
    while (tail != head) {
        slot_t* s = &s_ring[tail % RING_N];
        if (!tud_network_can_xmit((uint16_t)(NETRAW_UDP_OVERHEAD + s->len))) {
            s_st.tx_blocked++;
            break;
        }
        // tud_network_xmit copies the frame into an NTB (xmit_cb) before
        // it returns, so the slot is free right after.
        tud_network_xmit(s, KIND_UDP);
        s_st.tx_frames++;
        s_st.tx_bytes += NETRAW_UDP_OVERHEAD + s->len;
        s_st.tx_udp++;
        s_st.tx_udp_bytes += s->len;
        tail++;
        __atomic_store_n(&s_tail, tail, __ATOMIC_RELEASE);
        freed = true;
    }
    if (freed) xSemaphoreGive(s_space);
}

static void usbnet_task(void* arg) {
    (void)arg;
    // Initialised here, so the controller's interrupt is allocated on
    // this task's core, and freed on it again by tusb_deinit().
    //
    // Double-buffer the bulk IN endpoint's TX FIFO (F-18). In slave mode
    // TinyUSB gives it room for exactly one 64-byte packet, so after each
    // packet the host's next IN token is NAKed until our ISR has written
    // the next one: the first blast measured 5.27 Mbit/s of ~9.4 possible.
    // Two packets let the ISR refill one while the other goes out.
    tud_configure_param_t const cfg = {
        .dwc2 = {.bm_double_buffered = 1u << (EP_IN & 0x0f), .vbus_sensing = CFG_TUD_VBUS_DETECT_HW},
    };
    tud_configure(0, TUD_CFGID_DWC2, &cfg);
    tusb_rhport_init_t const init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    s_init_ok                     = tusb_rhport_init(0, &init);
    xSemaphoreGive(s_done);
    if (!s_init_ok) {
        vTaskDelete(NULL);
        return;
    }

    while (!s_stop) {
        if (s_poll) {
            taskYIELD();  // lower-priority tasks on this core starve; equal ones share
            s_st.wake_poll++;
        } else if (xSemaphoreTake(s_wake, WAKE_TICKS) == pdTRUE) {
            s_st.wake_event++;
        } else {
            s_st.wake_timeout++;
        }
        int64_t const t0 = esp_timer_get_time();
        tud_task_ext(0, false);  // everything queued, without blocking
        drain();
        s_st.task_busy_us += (uint64_t)(esp_timer_get_time() - t0);
    }

    // Leave the bus cleanly, so the PC sees an unplug, not a dead device.
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    tusb_deinit(0);
    s_mounted = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// --- public ---------------------------------------------------------------------------

static void free_all(void) {
    if (s_wake) vSemaphoreDelete(s_wake);
    if (s_space) vSemaphoreDelete(s_space);
    if (s_done) vSemaphoreDelete(s_done);
    if (s_prod) vSemaphoreDelete(s_prod);
    s_wake = s_space = s_done = s_prod = NULL;
    heap_caps_free(s_ring);
    heap_caps_free(s_replies);
    s_ring    = NULL;
    s_replies = NULL;
}

esp_err_t usbnet_start(usbnet_udp_cb_t on_udp, bool poll) {
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_ring    = heap_caps_calloc(RING_N, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    s_replies = heap_caps_calloc(REPLY_N, sizeof(reply_t), MALLOC_CAP_SPIRAM);
    s_wake    = xSemaphoreCreateBinary();
    s_space   = xSemaphoreCreateBinary();
    s_done    = xSemaphoreCreateBinary();
    s_prod    = xSemaphoreCreateMutex();
    if (!s_ring || !s_replies || !s_wake || !s_space || !s_done || !s_prod) {
        free_all();
        return ESP_ERR_NO_MEM;
    }
    s_head = s_tail = 0;
    s_reply_head = s_reply_tail = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_on_udp  = on_udp;
    s_poll    = poll;
    s_stop    = false;
    s_mounted = false;

    usbnet_macs(s_host_mac, s_dev_mac);
    snprintf(s_mac_str, sizeof(s_mac_str), "%02X%02X%02X%02X%02X%02X", s_host_mac[0], s_host_mac[1], s_host_mac[2],
             s_host_mac[3], s_host_mac[4], s_host_mac[5]);
    uint8_t const dev_ip[4]  = USBNET_DEV_IP;
    uint8_t const host_ip[4] = USBNET_HOST_IP;
    netraw_init(&s_net, s_dev_mac, s_host_mac, dev_ip, host_ip);

    // The console is about to go: flush it, then keep everything quiet
    // until it is back (a write to a console with no host costs time).
    // stdout reaches the console's FIFO directly (not through the driver
    // debugcon installed), so wait_tx_done does not cover it: give the
    // host a moment to fetch the last line (START) before the bus goes.
    fflush(stdout);
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_log_level_set("*", ESP_LOG_NONE);

    phy_to_otg();

    usb_phy_config_t const phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    esp_err_t res = usb_new_phy(&phy_conf, &s_phy);
    if (res != ESP_OK) goto fail_phy;

    // Polling runs at idle priority: it shares core 1 round-robin with the
    // idle task (so the watchdog stays fed), and the generator (priority 5,
    // same core) preempts it whenever it has data to queue.
    UBaseType_t const prio = poll ? tskIDLE_PRIORITY : TASK_PRIO;
    if (xTaskCreatePinnedToCore(usbnet_task, "usbnet", TASK_STACK, NULL, prio, &s_task, TASK_CORE) != pdPASS) {
        res = ESP_ERR_NO_MEM;
        goto fail_task;
    }
    if (xSemaphoreTake(s_done, START_TIMEOUT) != pdTRUE || !s_init_ok) {
        res = ESP_FAIL;  // the task deleted itself if init failed
        goto fail_task;
    }
    s_running = true;
    return ESP_OK;

fail_task:
    usb_del_phy(s_phy);
    s_phy = NULL;
fail_phy:
    phy_to_usj();
    esp_log_level_set("*", ESP_LOG_INFO);
    free_all();
    return res;
}

void usbnet_stop(void) {
    if (!s_running) return;
    s_stop = true;
    xSemaphoreGive(s_wake);
    xSemaphoreTake(s_done, START_TIMEOUT);
    s_running = false;
    // Wake any producer still waiting for room; it sees !s_running.
    xSemaphoreGive(s_space);

    usb_del_phy(s_phy);
    s_phy = NULL;
    phy_to_usj();
    esp_log_level_set("*", ESP_LOG_INFO);

    // Producers could still hold the mutex for an instant.
    xSemaphoreTake(s_prod, pdMS_TO_TICKS(100));
    xSemaphoreGive(s_prod);
    free_all();
}

bool usbnet_running(void) {
    return s_running;
}

bool usbnet_send_udp(uint16_t port, void const* data, uint16_t len, TickType_t wait) {
    if (!s_running || len > NETRAW_UDP_MAX) return false;
    if (!s_mounted) {
        s_st.tx_no_link++;
        return false;
    }
    TickType_t const start = xTaskGetTickCount();
    xSemaphoreTake(s_prod, portMAX_DELAY);
    uint32_t const head = s_head;
    for (;;) {
        uint32_t const tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
        if (head - tail < RING_N) break;
        TickType_t const spent = xTaskGetTickCount() - start;
        if (!s_running || spent >= wait) {
            s_st.tx_ring_full++;
            xSemaphoreGive(s_prod);
            return false;
        }
        xSemaphoreTake(s_space, wait - spent);
    }
    slot_t* s = &s_ring[head % RING_N];
    s->port   = port;
    s->len    = len;
    memcpy(s->data, data, len);
    __atomic_store_n(&s_head, head + 1, __ATOMIC_RELEASE);
    uint32_t const waiting = head + 1 - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    if (waiting > s_st.ring_peak) s_st.ring_peak = waiting;
    xSemaphoreGive(s_prod);
    xSemaphoreGive(s_wake);
    return true;
}

void usbnet_get_stats(usbnet_stats_t* out) {
    *out           = s_st;
    out->mounted   = s_mounted;
    out->net       = s_net.st;
    out->peer_seen = s_net.peer_seen;
}
