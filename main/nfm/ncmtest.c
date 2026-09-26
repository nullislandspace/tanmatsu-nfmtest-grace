// =====================================================================
//  ncmtest  --  the USB network link alone (see ncmtest.h)
// =====================================================================

#include "ncmtest.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "report.h"
#include "testkit/debugcon.h"
#include "usbnet.h"

#define GEN_STACK 4096
#define GEN_PRIO  5
#define GEN_CORE  1

#define PERIOD_MAX  600  // records kept (10 minutes)
#define PERIOD_JSON 512
#define SD_DIR      "/sd/nfmtest"

#define ACK_WINDOW_MS  20000
#define ACK_REPEAT_MS  2000
#define USB_SETTLE_MS  1500  // console back on the bus -> host has /dev/ttyACM0 again

#define BLAST_HDR 24

// --- parameters --------------------------------------------------------------

bool ncmtest_parse(char const* args, ncmtest_params_t* p, char* err, int err_len) {
    memset(p, 0, sizeof(*p));
    p->blast = true;
    p->secs  = 30;
    p->len   = 1316;
    snprintf(p->run, sizeof(p->run), "r%08lx", (unsigned long)(esp_timer_get_time() & 0xffffffff));

    char buf[256];
    strlcpy(buf, args ? args : "", sizeof(buf));
    char* save = NULL;
    for (char* w = strtok_r(buf, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        char* eq = strchr(w, '=');
        if (!eq) {
            snprintf(err, err_len, "expected key=value, got '%s'", w);
            return false;
        }
        *eq             = '\0';
        char const* key = w;
        char const* val = eq + 1;
        if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "blast") == 0) {
                p->blast = true;
            } else if (strcmp(val, "idle") == 0) {
                p->blast = false;
            } else {
                snprintf(err, err_len, "mode is blast or idle, not '%s'", val);
                return false;
            }
        } else if (strcmp(key, "secs") == 0) {
            p->secs = atoi(val);
            if (p->secs < 1 || p->secs > PERIOD_MAX) {
                snprintf(err, err_len, "secs must be 1..%d", PERIOD_MAX);
                return false;
            }
        } else if (strcmp(key, "len") == 0) {
            int const l = atoi(val);
            if (l < BLAST_HDR || l > NETRAW_UDP_MAX) {
                snprintf(err, err_len, "len must be %d..%d", BLAST_HDR, NETRAW_UDP_MAX);
                return false;
            }
            p->len = (uint16_t)l;
        } else if (strcmp(key, "rate") == 0) {
            p->rate_kbit = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "poll") == 0) {
            p->poll = atoi(val) != 0;
        } else if (strcmp(key, "run") == 0) {
            strlcpy(p->run, val, sizeof(p->run));
        } else {
            snprintf(err, err_len, "unknown key '%s'", key);
            return false;
        }
    }
    return true;
}

// --- the generator -----------------------------------------------------------------

typedef struct {
    ncmtest_params_t const* p;
    uint32_t                run_hash;
    volatile bool           stop;
    volatile bool           done;
    volatile uint32_t       seq;       // next sequence number
    volatile uint32_t       rejected;  // usbnet_send_udp() said no
} gen_t;

static uint32_t fnv1a(char const* s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static inline void put32(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);  // little-endian, as the PC reads it
}

static void gen_task(void* arg) {
    gen_t* const g   = arg;
    uint16_t const len = g->p->len;
    uint8_t* const buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        g->done = true;
        vTaskDelete(NULL);
        return;
    }
    int64_t const t0         = esp_timer_get_time();
    uint64_t      sent_bytes = 0;
    while (!g->stop) {
        if (g->p->rate_kbit > 0) {
            // Pace: payload bits sent so far may not run ahead of the clock.
            int64_t const  el      = esp_timer_get_time() - t0;
            uint64_t const allowed = (uint64_t)el * g->p->rate_kbit / 8000u;
            if (sent_bytes + len > allowed) {
                vTaskDelay(1);
                continue;
            }
        }
        uint32_t const seq = g->seq;
        memcpy(buf, "NFMB", 4);
        put32(buf + 4, g->run_hash);
        put32(buf + 8, seq);
        put32(buf + 12, len);
        uint64_t const now = (uint64_t)esp_timer_get_time();
        memcpy(buf + 16, &now, 8);
        for (uint16_t i = BLAST_HDR; i < len; i++) buf[i] = (uint8_t)(seq + i);
        if (usbnet_send_udp(NCMTEST_PORT_DATA, buf, len, pdMS_TO_TICKS(100))) {
            g->seq = seq + 1;
            sent_bytes += len;
        } else {
            g->rejected++;
            // No link yet, or the ring stayed full for 100 ms: do not spin.
            vTaskDelay(1);
        }
    }
    heap_caps_free(buf);
    g->done = true;
    vTaskDelete(NULL);
}

// --- records -------------------------------------------------------------------------

typedef struct {
    uint32_t sram, sram_big, sram_min, dma, dma_big;
} ledger_t;

static void ledger_take(ledger_t* l) {
    l->sram     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    l->sram_big = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    l->sram_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    l->dma      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    l->dma_big  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

static int ledger_json(char* out, size_t cap, char const* name, ledger_t const* l) {
    return snprintf(out, cap, "\"%s\":{\"sram\":%lu,\"sram_big\":%lu,\"sram_min\":%lu,\"dma\":%lu,\"dma_big\":%lu}",
                    name, (unsigned long)l->sram, (unsigned long)l->sram_big, (unsigned long)l->sram_min,
                    (unsigned long)l->dma, (unsigned long)l->dma_big);
}

static char s_mac_host[18], s_mac_dev[18];

static void mac_str(char out[18], uint8_t const m[6]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// --- control from the PC ---------------------------------------------------------------

static volatile bool s_stop_requested;

static void on_udp(netraw_udp_t const* u) {
    if (u->dst_port == NCMTEST_PORT_CTRL && u->len >= 4 && memcmp(u->data, "STOP", 4) == 0) s_stop_requested = true;
}

// --- the run ------------------------------------------------------------------------------

static void emit_all(char (*periods)[PERIOD_JSON], int n, char const* result) {
    for (int i = 0; i < n; i++) report_emit("PERIOD", periods[i]);
    report_emit("RESULT", result);
}

static void save_sd(char const* run, char const* start, char (*periods)[PERIOD_JSON], int n, char const* result) {
    mkdir(SD_DIR, 0777);  // EEXIST is fine
    char path[96];
    snprintf(path, sizeof(path), SD_DIR "/%s.json", run);
    FILE* f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "{\"start\":%s,\n\"periods\":[\n", start);
    for (int i = 0; i < n; i++) fprintf(f, "%s%s\n", periods[i], i + 1 < n ? "," : "");
    fprintf(f, "],\n\"result\":%s}\n", result);
    fclose(f);
}

void ncmtest_run(ncmtest_params_t const* p, ncmtest_hud_t hud, bool console) {
    static char start_json[512];
    static char result_json[REPORT_JSON_MAX];
    char (*periods)[PERIOD_JSON] = heap_caps_calloc(PERIOD_MAX, PERIOD_JSON, MALLOC_CAP_SPIRAM);
    if (periods == NULL) {
        report_emitf("RESULT", "{\"status\":\"error\",\"why\":\"no memory for records\"}");
        return;
    }

    uint8_t host_mac[6], dev_mac[6];
    usbnet_macs(host_mac, dev_mac);
    mac_str(s_mac_host, host_mac);
    mac_str(s_mac_dev, dev_mac);

    ledger_t l_before, l_up, l_after;
    ledger_take(&l_before);

    snprintf(start_json, sizeof(start_json),
             "{\"run\":\"%s\",\"mode\":\"%s\",\"secs\":%d,\"len\":%u,\"rate\":%lu,\"host_mac\":\"%s\","
             "\"dev_mac\":\"%s\",\"host_ip\":\"192.168.77.1\",\"dev_ip\":\"192.168.77.2\",\"port\":%d,"
             "\"stats_port\":%d,\"ctrl_port\":%d,\"run_hash\":%lu,\"poll\":%d}",
             p->run, p->blast ? "blast" : "idle", p->secs, (unsigned)p->len, (unsigned long)p->rate_kbit, s_mac_host,
             s_mac_dev, NCMTEST_PORT_DATA, NCMTEST_PORT_STATS, NCMTEST_PORT_CTRL, (unsigned long)fnv1a(p->run), p->poll ? 1 : 0);
    report_emit("START", start_json);

    char        l0[64], l1[64], l2[64], l3[64], l4[64], l5[64], l6[64], l7[64];
    char const* lines[] = {l0, l1, l2, l3, l4, l5, l6, l7};
    snprintf(l0, sizeof(l0), "NCM %s%s  run %s", p->blast ? "blast" : "idle", p->poll ? " POLL" : "", p->run);
    snprintf(l1, sizeof(l1), "PC side %s = 192.168.77.1", s_mac_host);
    snprintf(l2, sizeof(l2), "switching USB-C to network mode...");
    l3[0] = l4[0] = l5[0] = l6[0] = l7[0] = '\0';
    if (hud) hud(lines, 8);

    s_stop_requested   = false;
    int64_t const t_on = esp_timer_get_time();
    esp_err_t const res = usbnet_start(on_udp, p->poll);
    int64_t const t_up = esp_timer_get_time();
    if (res != ESP_OK) {
        // The console is back already (usbnet_start undoes its steps).
        snprintf(result_json, sizeof(result_json), "{\"run\":\"%s\",\"status\":\"error\",\"why\":\"usbnet_start: %s\"}",
                 p->run, esp_err_to_name(res));
        report_emit("RESULT", result_json);
        heap_caps_free(periods);
        return;
    }

    gen_t gen = {.p = p, .run_hash = fnv1a(p->run)};
    if (p->blast) xTaskCreatePinnedToCore(gen_task, "nfm_gen", GEN_STACK, &gen, GEN_PRIO, NULL, GEN_CORE);

    // Once a second: a PERIOD record, kept and sent to the PC.
    int            n_periods = 0;
    int64_t        mount_us  = -1;
    int64_t        dhcp_us   = -1;
    usbnet_stats_t prev      = {0};
    int64_t        t_prev    = esp_timer_get_time();
    int64_t const  t_end     = t_prev + (int64_t)p->secs * 1000000;
    int64_t        t_hud     = 0;
    bool           ledger_up_taken = false;
    uint32_t       gen_rej_prev    = 0;

    while (!s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t const  now = esp_timer_get_time();
        usbnet_stats_t st;
        usbnet_get_stats(&st);
        if (mount_us < 0 && st.mounted) mount_us = now - t_on;
        if (dhcp_us < 0 && st.net.dhcp_acked > 0) dhcp_us = now - t_on;
        if (!ledger_up_taken && st.mounted) {
            ledger_take(&l_up);
            ledger_up_taken = true;
        }

        if (now - t_prev >= 1000000 || now >= t_end) {
            double const   dt   = (double)(now - t_prev) / 1e6;
            uint64_t const dudp = st.tx_udp_bytes - prev.tx_udp_bytes;
            uint64_t const deth = st.tx_bytes - prev.tx_bytes;
            double const   busy = (double)(st.task_busy_us - prev.task_busy_us) / (double)(now - t_prev) * 100.0;
            uint32_t const rej  = gen.rejected;
            if (n_periods < PERIOD_MAX) {
                char* j = periods[n_periods];
                snprintf(j, PERIOD_JSON,
                         "{\"i\":%d,\"t\":%.3f,\"mounted\":%d,\"udp_mbit\":%.3f,\"eth_mbit\":%.3f,\"udp\":%lu,"
                         "\"seq\":%lu,\"blocked\":%lu,\"ring_full\":%lu,\"no_link\":%lu,\"gen_rej\":%lu,\"peak\":%lu,"
                         "\"rx\":%lu,\"arp\":%lu,\"icmp\":%lu,\"dhcp\":%lu,\"udp_in\":%lu,\"ign\":%lu,\"bad\":%lu,"
                         "\"usb_busy_pct\":%.2f,\"wk_ev\":%lu,\"wk_to\":%lu,\"wk_poll\":%lu,\"hk_isr\":%lu,"
                         "\"hk_task\":%lu,\"xfer\":%lu,\"sram\":%u,\"sram_big\":%u}",
                         n_periods, (double)(now - t_on) / 1e6, st.mounted ? 1 : 0, (double)dudp * 8 / dt / 1e6,
                         (double)deth * 8 / dt / 1e6, (unsigned long)(st.tx_udp - prev.tx_udp), (unsigned long)gen.seq,
                         (unsigned long)(st.tx_blocked - prev.tx_blocked),
                         (unsigned long)(st.tx_ring_full - prev.tx_ring_full),
                         (unsigned long)(st.tx_no_link - prev.tx_no_link), (unsigned long)(rej - gen_rej_prev),
                         (unsigned long)st.ring_peak, (unsigned long)(st.rx_frames - prev.rx_frames),
                         (unsigned long)st.net.rx_arp, (unsigned long)st.net.rx_icmp, (unsigned long)st.net.rx_dhcp,
                         (unsigned long)st.net.rx_udp, (unsigned long)st.net.rx_ignored, (unsigned long)st.net.rx_bad,
                         busy, (unsigned long)(st.wake_event - prev.wake_event),
                         (unsigned long)(st.wake_timeout - prev.wake_timeout),
                         (unsigned long)(st.wake_poll - prev.wake_poll), (unsigned long)(st.hook_isr - prev.hook_isr),
                         (unsigned long)(st.hook_task - prev.hook_task),
                         (unsigned long)(st.xfer_complete - prev.xfer_complete),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                usbnet_send_udp(NCMTEST_PORT_STATS, j, (uint16_t)strlen(j), 0);
                n_periods++;
            }
            snprintf(l3, sizeof(l3), "%s  %.2f Mbit/s UDP  usb task %.1f%%", st.mounted ? "mounted" : "waiting for host",
                     (double)dudp * 8 / dt / 1e6, busy);
            snprintf(l6, sizeof(l6), "per s: wake ev %lu  timeout %lu  poll %lu",
                     (unsigned long)(st.wake_event - prev.wake_event),
                     (unsigned long)(st.wake_timeout - prev.wake_timeout),
                     (unsigned long)(st.wake_poll - prev.wake_poll));
            snprintf(l7, sizeof(l7), "per s: hook isr %lu  task %lu  xfer done %lu",
                     (unsigned long)(st.hook_isr - prev.hook_isr), (unsigned long)(st.hook_task - prev.hook_task),
                     (unsigned long)(st.xfer_complete - prev.xfer_complete));
            prev         = st;
            gen_rej_prev = rej;
            t_prev       = now;
        }
        if (now - t_hud >= 500000) {
            snprintf(l2, sizeof(l2), "%d / %d s   (STOP via UDP :%d)", (int)((now - t_on) / 1000000), p->secs,
                     NCMTEST_PORT_CTRL);
            snprintf(l4, sizeof(l4), "rx %lu  arp %lu  ping %lu  dhcp %lu  echo %lu", (unsigned long)st.rx_frames,
                     (unsigned long)st.net.rx_arp, (unsigned long)st.net.rx_icmp, (unsigned long)st.net.rx_dhcp,
                     (unsigned long)st.net.rx_udp);
            snprintf(l5, sizeof(l5), "sent %lu  blocked %lu  full %lu  nolink %lu", (unsigned long)st.tx_udp,
                     (unsigned long)st.tx_blocked, (unsigned long)st.tx_ring_full, (unsigned long)st.tx_no_link);
            if (hud) hud(lines, 8);
            t_hud = now;
        }
        if (now >= t_end) break;
    }

    // Stop the generator, let the ring drain a moment, then take the link down.
    gen.stop = true;
    for (int i = 0; p->blast && !gen.done && i < 100; i++) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(100));
    usbnet_stats_t st;
    usbnet_get_stats(&st);
    int64_t const t_down0 = esp_timer_get_time();
    usbnet_stop();
    int64_t const t_down = esp_timer_get_time();
    ledger_take(&l_after);

    double const secs_run = (double)(t_down0 - t_up) / 1e6;
    bool const   ok       = st.mounts > 0 && (!p->blast || st.tx_udp > 0);
    char         lb[160], lu[160], la[160];
    ledger_json(lb, sizeof(lb), "before", &l_before);
    ledger_json(la, sizeof(la), "after", &l_after);
    if (ledger_up_taken) {
        ledger_json(lu, sizeof(lu), "up", &l_up);
    } else {
        snprintf(lu, sizeof(lu), "\"up\":null");
    }
    snprintf(result_json, sizeof(result_json),
             "{\"run\":\"%s\",\"status\":\"%s\",\"mode\":\"%s\",\"stopped_by_pc\":%d,\"secs\":%.3f,"
             "\"switch_on_ms\":%.1f,\"switch_off_ms\":%.1f,\"mount_ms\":%.1f,\"dhcp_ms\":%.1f,\"mounts\":%lu,"
             "\"unmounts\":%lu,\"peer_seen\":%d,\"seq\":%lu,\"tx_udp\":%lu,\"tx_udp_bytes\":%llu,\"tx_frames\":%lu,"
             "\"tx_bytes\":%llu,\"udp_mbit\":%.3f,\"blocked\":%lu,\"ring_full\":%lu,\"no_link\":%lu,\"gen_rej\":%lu,"
             "\"peak\":%lu,\"rx\":%lu,\"rx_replies_dropped\":%lu,\"arp\":%lu,\"icmp\":%lu,\"dhcp\":%lu,"
             "\"dhcp_acked\":%lu,\"udp_in\":%lu,\"ign\":%lu,\"bad\":%lu,\"replies\":%lu,\"usb_busy_pct\":%.2f,"
             "\"poll\":%d,\"wk_ev\":%lu,\"wk_to\":%lu,\"wk_poll\":%lu,\"hk_isr\":%lu,\"hk_task\":%lu,\"xfer\":%lu,"
             "\"periods\":%d,\"ledger\":{%s,%s,%s}}",
             p->run, ok ? "ok" : "bad", p->blast ? "blast" : "idle", s_stop_requested ? 1 : 0, secs_run,
             (double)(t_up - t_on) / 1e3, (double)(t_down - t_down0) / 1e3, mount_us < 0 ? -1.0 : mount_us / 1e3,
             dhcp_us < 0 ? -1.0 : dhcp_us / 1e3, (unsigned long)st.mounts, (unsigned long)st.unmounts,
             st.peer_seen ? 1 : 0, (unsigned long)gen.seq, (unsigned long)st.tx_udp,
             (unsigned long long)st.tx_udp_bytes, (unsigned long)st.tx_frames, (unsigned long long)st.tx_bytes,
             secs_run > 0 ? (double)st.tx_udp_bytes * 8 / secs_run / 1e6 : 0.0, (unsigned long)st.tx_blocked,
             (unsigned long)st.tx_ring_full, (unsigned long)st.tx_no_link, (unsigned long)gen.rejected,
             (unsigned long)st.ring_peak, (unsigned long)st.rx_frames, (unsigned long)st.rx_replies_dropped,
             (unsigned long)st.net.rx_arp, (unsigned long)st.net.rx_icmp, (unsigned long)st.net.rx_dhcp,
             (unsigned long)st.net.dhcp_acked, (unsigned long)st.net.rx_udp, (unsigned long)st.net.rx_ignored,
             (unsigned long)st.net.rx_bad, (unsigned long)st.net.tx_replies,
             secs_run > 0 ? (double)st.task_busy_us / (secs_run * 1e4) : 0.0, p->poll ? 1 : 0,
             (unsigned long)st.wake_event, (unsigned long)st.wake_timeout, (unsigned long)st.wake_poll,
             (unsigned long)st.hook_isr, (unsigned long)st.hook_task, (unsigned long)st.xfer_complete, n_periods, lb,
             lu, la);

    save_sd(p->run, start_json, periods, n_periods, result_json);

    snprintf(l2, sizeof(l2), "done: %s, console back", ok ? "ok" : "BAD");
    snprintf(l3, sizeof(l3), "%.2f Mbit/s UDP avg, %lu datagrams", secs_run > 0 ? (double)st.tx_udp_bytes * 8 / secs_run / 1e6 : 0.0,
             (unsigned long)st.tx_udp);
    if (hud) hud(lines, 8);

    // The host needs a moment to find /dev/ttyACM0 again; then repeat
    // until it says it has everything.
    vTaskDelay(pdMS_TO_TICKS(USB_SETTLE_MS));
    debugcon_take_ack();  // a stale one does not count
    if (!console) {
        emit_all(periods, n_periods, result_json);
    } else {
        int64_t const deadline = esp_timer_get_time() + (int64_t)ACK_WINDOW_MS * 1000;
        bool          acked    = false;
        while (!acked && esp_timer_get_time() < deadline) {
            emit_all(periods, n_periods, result_json);
            for (int i = 0; i < ACK_REPEAT_MS / 50 && !acked; i++) {
                vTaskDelay(pdMS_TO_TICKS(50));
                acked = debugcon_take_ack();
            }
        }
    }
    heap_caps_free(periods);
}
