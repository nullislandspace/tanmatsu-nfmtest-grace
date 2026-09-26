// =====================================================================
//  stream  --  framebuffer -> PPA -> H.264 -> MPEG-TS -> UDP (stream.h)
// =====================================================================

#include "stream.h"
#include <string.h>
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_types.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "tsmux.h"
#include "usbnet.h"

#define OUT_W     800
#define OUT_H     480
#define YUV_BYTES (OUT_W * OUT_H * 3 / 2)
#define NBUF      3
#define PORT      5000

#define RENDER_PRIO  3
#define RENDER_CORE  0
#define STREAM_PRIO  5  // below usbnet (10), on its core (Part B)
#define STREAM_CORE  1
#define TASK_STACK   6144
#define SEND_WAIT    2  // ticks a datagram may wait for room in the ring

static stream_cfg_t          s_cfg;
static stream_stats_t        s_st;
static void (*s_blit)(void const*);
static pax_buf_t             s_fb[NBUF];
static void*                 s_mem[NBUF];
static size_t                s_fb_bytes;
static uint8_t*              s_yuv;
static uint8_t*              s_bs;
static ppa_client_handle_t   s_ppa;
static esp_h264_enc_handle_t s_enc;
static tsmux_t               s_mux;
static volatile bool         s_run;
static volatile int          s_done;       // tasks that have finished
static volatile int          s_published;  // newest complete frame, -1 none
static volatile int          s_capturing;  // being read by the PPA, -1 none

static bool emit(void* ctx, uint8_t const* d, size_t len) {
    (void)ctx;
    return usbnet_send_udp(PORT, d, (uint16_t)len, SEND_WAIT);
}

// --- render: draw into a buffer nobody else is using, then publish it ----

static void render_task(void* arg) {
    (void)arg;
    uint32_t n = 0;
    while (s_run) {
        int b = 0;
        // Neither the one the stream may grab next nor the one it reads.
        __sync_synchronize();
        while (b == s_published || b == s_capturing) b++;
        pattern_draw(&s_fb[b], s_cfg.pat, n++, (uint32_t)s_cfg.fps);
        esp_cache_msync(s_mem[b], s_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_published = b;
        __sync_synchronize();
        s_st.rendered++;
        if (s_blit) s_blit(s_mem[b]);
        vTaskDelay(1);  // let the idle task run
    }
    s_done++;
    vTaskDelete(NULL);
}

// --- stream: fixed rate, newest frame, never waits for the renderer --------

static void stream_task(void* arg) {
    (void)arg;
    int64_t const period = 1000000 / s_cfg.fps;
    int64_t       next   = esp_timer_get_time();
    uint32_t      n      = 0;  // stream frame number: PTS = n / fps
    while (s_run) {
        int64_t now = esp_timer_get_time();
        if (now < next) {
            int64_t const wait_ms = (next - now) / 1000;
            vTaskDelay(wait_ms >= 10 ? pdMS_TO_TICKS(wait_ms) : 1);
            continue;
        }
        // Slots already gone by are skipped, not made up for.
        while (next + period <= now) {
            next += period;
            n++;
            s_st.skipped++;
        }
        next += period;

        // Claim the newest frame. The renderer publishes before it picks
        // its next buffer, so if `published` still matches after the claim
        // is visible, it cannot pick this one.
        int b;
        do {
            b           = s_published;
            s_capturing = b;
            __sync_synchronize();
        } while (b != s_published);
        if (b < 0) continue;
        int64_t const t0 = esp_timer_get_time();

        ppa_srm_oper_config_t const srm = {
            .in =
                {
                    .buffer  = s_mem[b],
                    .pic_w   = (uint32_t)pax_buf_get_width_raw(&s_fb[b]),
                    .pic_h   = (uint32_t)pax_buf_get_height_raw(&s_fb[b]),
                    .block_w = (uint32_t)pax_buf_get_width_raw(&s_fb[b]),
                    .block_h = (uint32_t)pax_buf_get_height_raw(&s_fb[b]),
                    .srm_cm  = PPA_SRM_COLOR_MODE_RGB565,
                },
            .out =
                {
                    .buffer      = s_yuv,
                    .buffer_size = YUV_BYTES,
                    .pic_w       = OUT_W,
                    .pic_h       = OUT_H,
                    .srm_cm      = PPA_SRM_COLOR_MODE_YUV420,
                    .yuv_range   = PPA_COLOR_RANGE_LIMIT,
                    .yuv_std     = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,  // undoes pax's PAX_O_ROT_CW (step 1.2)
            .scale_x        = 1.0f,
            .scale_y        = 1.0f,
            .mode           = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t const perr = ppa_do_scale_rotate_mirror(s_ppa, &srm);
        s_capturing          = -1;
        int64_t const t1     = esp_timer_get_time();
        if (perr != ESP_OK) {
            s_st.enc_errors++;
            n++;
            continue;
        }

        uint64_t const           pts = 90000 + (uint64_t)n * 90000 / (uint64_t)s_cfg.fps;
        esp_h264_enc_in_frame_t  in  = {.raw_data = {.buffer = s_yuv, .len = YUV_BYTES}, .pts = (uint32_t)pts};
        esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = s_bs, .len = YUV_BYTES}};
        esp_h264_err_t const     herr = esp_h264_enc_process(s_enc, &in, &out);
        int64_t const            t2   = esp_timer_get_time();
        if (herr != ESP_H264_ERR_OK || out.length == 0) {
            s_st.enc_errors++;
            n++;
            continue;
        }
        bool const key = out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I;
        tsmux_write(&s_mux, s_bs, out.length, pts, key);
        int64_t const t3 = esp_timer_get_time();

        s_st.frames++;
        if (key) s_st.keyframes++;
        s_st.es_bytes += out.length;
        s_st.dgrams        = s_mux.dgrams;
        s_st.dgrams_failed = s_mux.dgrams_failed;
        s_st.ts_bytes      = s_mux.bytes;
        uint32_t const ppa = (uint32_t)(t1 - t0), enc = (uint32_t)(t2 - t1), mux = (uint32_t)(t3 - t2);
        s_st.ppa_us_sum += ppa;
        s_st.enc_us_sum += enc;
        s_st.mux_us_sum += mux;
        if (ppa > s_st.ppa_us_max) s_st.ppa_us_max = ppa;
        if (enc > s_st.enc_us_max) s_st.enc_us_max = enc;
        if (mux > s_st.mux_us_max) s_st.mux_us_max = mux;
        n++;
    }
    s_done++;
    vTaskDelete(NULL);
}

// --- set up and tear down ---------------------------------------------------

static void free_all(void) {
    if (s_enc) {
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
    }
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    for (int i = 0; i < NBUF; i++) {
        if (s_mem[i]) pax_buf_destroy(&s_fb[i]);
        heap_caps_free(s_mem[i]);
        s_mem[i] = NULL;
    }
    heap_caps_free(s_yuv);
    heap_caps_free(s_bs);
    s_yuv = s_bs = NULL;
}

esp_err_t stream_prepare(stream_cfg_t const* cfg, pax_buf_t const* tmpl, void (*blit)(void const* pixels)) {
    s_cfg = *cfg;
    if (s_cfg.gop < 1) s_cfg.gop = s_cfg.fps;
    s_blit = blit;
    memset(&s_st, 0, sizeof(s_st));

    int const w = pax_buf_get_width_raw(tmpl), h = pax_buf_get_height_raw(tmpl);
    if (pax_buf_get_type(tmpl) != PAX_BUF_16_565RGB || w * h != OUT_W * OUT_H) return ESP_ERR_NOT_SUPPORTED;
    s_fb_bytes = (size_t)w * (size_t)h * 2;
    for (int i = 0; i < NBUF; i++) {
        s_mem[i] = heap_caps_aligned_calloc(64, 1, s_fb_bytes, MALLOC_CAP_SPIRAM);
        if (!s_mem[i]) goto nomem;
        pax_buf_init(&s_fb[i], s_mem[i], w, h, PAX_BUF_16_565RGB);
        pax_buf_set_orientation(&s_fb[i], pax_buf_get_orientation(tmpl));
    }
    s_yuv = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);
    s_bs  = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);  // >= input, or the encoder refuses
    if (!s_yuv || !s_bs) goto nomem;

    ppa_client_config_t const pcfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&pcfg, &s_ppa) != ESP_OK) {
        free_all();
        return ESP_FAIL;
    }
    esp_h264_enc_cfg_hw_t const ecfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = (uint8_t)s_cfg.gop,
        .fps      = (uint8_t)s_cfg.fps,
        .res      = {.width = OUT_W, .height = OUT_H},
        .rc       = {.bitrate = s_cfg.br_kbit * 1000u, .qp_min = 16, .qp_max = 40},
    };
    if (esp_h264_enc_hw_new(&ecfg, &s_enc) != ESP_H264_ERR_OK || s_enc == NULL ||
        esp_h264_enc_open(s_enc) != ESP_H264_ERR_OK) {
        free_all();
        return ESP_FAIL;
    }
    tsmux_init(&s_mux, emit, NULL);
    return ESP_OK;

nomem:
    free_all();
    return ESP_ERR_NO_MEM;
}

void stream_start(void) {
    s_published = -1;
    s_capturing = -1;
    s_done      = 0;
    s_run       = true;
    xTaskCreatePinnedToCore(render_task, "nfm_render", TASK_STACK, NULL, RENDER_PRIO, NULL, RENDER_CORE);
    xTaskCreatePinnedToCore(stream_task, "nfm_stream", TASK_STACK, NULL, STREAM_PRIO, NULL, STREAM_CORE);
}

void stream_stop(void) {
    s_run = false;
    // The render task can be in the middle of a 100+ ms pattern.
    for (int i = 0; s_done < 2 && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free_all();
}

void stream_get_stats(stream_stats_t* out) {
    *out = s_st;
}
