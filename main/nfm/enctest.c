// =====================================================================
//  enctest  --  RGB565 framebuffer -> PPA -> HW H.264 (see enctest.h)
// =====================================================================

#include "enctest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_types.h"
#include "esp_heap_caps.h"
#include "hw_ver1/h264_dma_struct.h"  // register layouts, for diag (the P4 here is pre-v3)
#include "hw_ver1/h264_struct.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "pattern.h"
#include "report.h"
#include "screenshot.h"

#define OUT_W      800
#define OUT_H      480
#define YUV_BYTES  (OUT_W * OUT_H * 3 / 2)  // 576 000: no padding, both are multiples of 16
#define FRAMES_MAX 1800
#define SD_DIR     "/sd/nfmtest"

enum { ST_DRAW, ST_SYNC, ST_PPA, ST_ENC, ST_WRITE, ST_SHOW, ST_TOTAL, ST_COUNT };
static char const* const ST_NAMES[ST_COUNT] = {"draw", "sync", "ppa", "enc", "write", "show", "total"};

typedef struct {
    int       frames;
    int       fps;
    uint32_t  br_kbit;
    int       gop;
    int       qp_min, qp_max;
    pattern_t pat;
    int       rot;
    bool      pace, show, save;
    int       diag;
    bool      flat;  // src=flat: feed the encoder a flat YUV frame, no PPA (diagnostics)
    int       w, h;  // encoder size with src=flat  // 0 off; 1 sample registers; 2 also mask the encoder's interrupts
    char      run[40];
} params_t;

typedef struct {
    uint32_t sram, sram_big, dma;
} ledger_t;

static void ledger_take(ledger_t* l) {
    l->sram     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    l->sram_big = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    l->dma      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

static int ledger_json(char* out, size_t cap, char const* name, ledger_t const* l) {
    return snprintf(out, cap, "\"%s\":{\"sram\":%lu,\"sram_big\":%lu,\"dma\":%lu}", name, (unsigned long)l->sram,
                    (unsigned long)l->sram_big, (unsigned long)l->dma);
}

static bool parse(char const* args, params_t* p, char* err, size_t err_len) {
    *p = (params_t){.w = OUT_W, .h = OUT_H, .frames = 90, .fps = 30, .br_kbit = 3000, .gop = -1, .qp_min = 16, .qp_max = 40,
                    .pat = PAT_MOTION, .rot = 90, .pace = true, .show = true, .save = true};
    snprintf(p->run, sizeof(p->run), "e%08lx", (unsigned long)(esp_timer_get_time() & 0xffffffff));
    char buf[256];
    strlcpy(buf, args ? args : "", sizeof(buf));
    char* save = NULL;
    for (char* w = strtok_r(buf, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        char* eq = strchr(w, '=');
        if (!eq) {
            snprintf(err, err_len, "expected key=value, got '%s'", w);
            return false;
        }
        *eq = '\0';
        char const* k = w;
        char const* v = eq + 1;
        if (strcmp(k, "frames") == 0) {
            p->frames = atoi(v);
        } else if (strcmp(k, "fps") == 0) {
            p->fps = atoi(v);
        } else if (strcmp(k, "br") == 0) {
            p->br_kbit = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(k, "gop") == 0) {
            p->gop = atoi(v);
        } else if (strcmp(k, "qp") == 0) {
            if (sscanf(v, "%d,%d", &p->qp_min, &p->qp_max) != 2) {
                snprintf(err, err_len, "qp=<min>,<max>");
                return false;
            }
        } else if (strcmp(k, "pat") == 0) {
            if (!pattern_parse(v, &p->pat)) {
                snprintf(err, err_len, "unknown pattern '%s'", v);
                return false;
            }
        } else if (strcmp(k, "rot") == 0) {
            p->rot = atoi(v);
        } else if (strcmp(k, "pace") == 0) {
            p->pace = atoi(v) != 0;
        } else if (strcmp(k, "show") == 0) {
            p->show = atoi(v) != 0;
        } else if (strcmp(k, "save") == 0) {
            p->save = atoi(v) != 0;
        } else if (strcmp(k, "src") == 0) {
            p->flat = strcmp(v, "flat") == 0;
        } else if (strcmp(k, "w") == 0) {
            p->w = atoi(v);
        } else if (strcmp(k, "h") == 0) {
            p->h = atoi(v);
        } else if (strcmp(k, "diag") == 0) {
            p->diag = atoi(v);
        } else if (strcmp(k, "run") == 0) {
            strlcpy(p->run, v, sizeof(p->run));
        } else {
            snprintf(err, err_len, "unknown key '%s'", k);
            return false;
        }
    }
    if (p->gop < 0) p->gop = p->fps;
    if (!p->flat) {
        p->w = OUT_W;
        p->h = OUT_H;
    }
    if (p->w < 16 || p->h < 16 || p->w > OUT_W || p->h > OUT_H || (p->w & 15) || (p->h & 15)) {
        snprintf(err, err_len, "w, h: multiples of 16 up to %dx%d", OUT_W, OUT_H);
        return false;
    }
    if (p->diag) {
        p->frames = 2;
        p->pace   = false;
        p->show   = false;
        p->save   = false;
    }
    if (p->frames < 1 || p->frames > FRAMES_MAX || p->fps < 1 || p->fps > 60 || p->gop < 1 || p->gop > 255 ||
        p->qp_min < 0 || p->qp_max > 51 || p->qp_min > p->qp_max ||
        (p->rot != 0 && p->rot != 90 && p->rot != 180 && p->rot != 270)) {
        snprintf(err, err_len, "out of range: frames 1..%d, fps 1..60, gop 1..255, qp 0..51, rot 0/90/180/270",
                 FRAMES_MAX);
        return false;
    }
    return true;
}

static int cmp_u32(void const* a, void const* b) {
    uint32_t const x = *(uint32_t const*)a, y = *(uint32_t const*)b;
    return x < y ? -1 : x > y;
}

// mean/p95/max of n values, as JSON. Sorts `v` in place.
static int stat_json(char* out, size_t cap, char const* name, uint32_t* v, int n) {
    if (n <= 0) return snprintf(out, cap, "\"%s\":null", name);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    qsort(v, (size_t)n, sizeof(uint32_t), cmp_u32);
    int const i95 = (n * 95 + 99) / 100 - 1;
    return snprintf(out, cap, "\"%s\":{\"mean\":%lu,\"p95\":%lu,\"max\":%lu}", name, (unsigned long)(sum / (uint64_t)n),
                    (unsigned long)v[i95 < 0 ? 0 : i95], (unsigned long)v[n - 1]);
}

static ppa_srm_rotation_angle_t ppa_angle(int rot) {
    switch (rot) {
        case 90: return PPA_SRM_ROTATION_ANGLE_90;
        case 180: return PPA_SRM_ROTATION_ANGLE_180;
        case 270: return PPA_SRM_ROTATION_ANGLE_270;
        default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

// --- diag=1: watch the encoder's registers while one frame encodes ------
//
// The first hardware run timed out on every frame: the encoder's
// frame-done interrupt never came. This samples the encoder's and its
// DMA's status registers while esp_h264_enc_process() runs in a helper
// task, and prints every change, to tell apart "never starts" (no raw
// interrupt bits), "interrupt not delivered" (raw bits set, handler never
// clears them) and "DMA stalls" (AXI errors, channel states stuck).

#define H264_HW     ((h264_dev_t*)0x50084000)
#define H264_DMA_HW ((h264_dma_dev_t*)0x500A7000)
#define DIAG_N      24

typedef struct {
    esp_h264_enc_handle_t    enc;
    esp_h264_enc_in_frame_t* in;
    esp_h264_enc_out_frame_t* out;
    volatile bool            done;
    volatile esp_h264_err_t  err;
} diag_job_t;

static void diag_task(void* arg) {
    diag_job_t* j = arg;
    j->err        = esp_h264_enc_process(j->enc, j->in, j->out);
    j->done       = true;
    vTaskDelete(NULL);
}

static void diag_sample(uint32_t v[DIAG_N]) {
    h264_dev_t volatile* const     h = H264_HW;
    h264_dma_dev_t volatile* const d = H264_DMA_HW;
    int                            i = 0;
    v[i++] = h->int_raw.val;
    v[i++] = h->int_st.val;
    v[i++] = h->int_ena.val;
    v[i++] = h->sys_status.val;
    v[i++] = h->frame_code_length.val;
    v[i++] = h->debug_info0.val;
    v[i++] = h->debug_info1.val;
    v[i++] = h->debug_info2.val;
    for (int c = 0; c < 5; c++) v[i++] = d->dma_out_ch[c].int_raw.val;
    for (int c = 0; c < 5; c++) v[i++] = d->dma_in_ch[c].int_raw.val;
    v[i++] = d->dma_in_ch5.int_raw.val;
    v[i++] = d->inter_axi_err.val;
    v[i++] = d->exter_axi_err.val;
    v[i++] = d->dma_out_ch[0].state.val;
    v[i++] = d->dma_in_ch[0].state.val;
    v[i++] = d->rx_ch0_counter.val;
}

static char const* const DIAG_NAMES[DIAG_N] = {
    "int_raw", "int_st",  "int_ena", "sys_status", "code_len", "dbg0",    "dbg1",       "dbg2",
    "o0_raw",  "o1_raw",  "o2_raw",  "o3_raw",     "o4_raw",   "i0_raw",  "i1_raw",     "i2_raw",
    "i3_raw",  "i4_raw",  "i5_raw",  "axi_int_err", "axi_ext_err", "o0_state", "i0_state", "rx0_cnt"};

static void diag_emit(char const* when, int64_t t_us, uint32_t const v[DIAG_N]) {
    char   j[900];
    size_t len = (size_t)snprintf(j, sizeof(j), "{\"when\":\"%s\",\"t_ms\":%.1f", when, t_us / 1000.0);
    for (int k = 0; k < DIAG_N && len < sizeof(j); k++)
        len += (size_t)snprintf(j + len, sizeof(j) - len, ",\"%s\":\"%08lx\"", DIAG_NAMES[k], (unsigned long)v[k]);
    if (len < sizeof(j)) snprintf(j + len, sizeof(j) - len, "}");
    report_emit("ENCDIAG", j);
}

static void diag_static(void) {
    h264_dma_dev_t volatile* const d = H264_DMA_HW;
    report_emitf("ENCDIAG",
                 "{\"when\":\"setup\",\"clk_sys_en\":%u,\"clk_en\":%u,\"clk_src\":%u,\"rst\":%u,"
                 "\"inter0\":[\"%08lx\",\"%08lx\"],\"inter1\":[\"%08lx\",\"%08lx\"],"
                 "\"exter0\":[\"%08lx\",\"%08lx\"],\"exter1\":[\"%08lx\",\"%08lx\"],\"dma_date\":\"%08lx\","
                 "\"h264_date\":\"%08lx\"}",
                 (unsigned)HP_SYS_CLKRST.soc_clk_ctrl1.reg_h264_sys_clk_en,
                 (unsigned)HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_en,
                 (unsigned)HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_src_sel,
                 (unsigned)HP_SYS_CLKRST.hp_rst_en2.reg_rst_en_h264, (unsigned long)d->inter_mem_addr[0].start.val,
                 (unsigned long)d->inter_mem_addr[0].end.val, (unsigned long)d->inter_mem_addr[1].start.val,
                 (unsigned long)d->inter_mem_addr[1].end.val, (unsigned long)d->exter_mem_addr[0].start.val,
                 (unsigned long)d->exter_mem_addr[0].end.val, (unsigned long)d->exter_mem_addr[1].start.val,
                 (unsigned long)d->exter_mem_addr[1].end.val, (unsigned long)d->date.val,
                 (unsigned long)H264_HW->date.val);
}

// What the encoder's DMA left in the output buffer. The buffer is filled
// with 0xAA before the encode; after it, read back past the cache: how far
// it was written, where the Annex-B start codes are, which NAL types.
// `code_len` is the encoder's own count (its register, sampled before the
// driver's timeout path resets it). Saved to SD for a decode attempt.
#define BS_FILL 0xAA

static void diag_bitstream(uint8_t* bs, size_t cap, uint32_t code_len, int frame, char const* run) {
    esp_cache_msync(bs, cap, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    size_t last = 0;  // one past the last byte that is not the fill
    for (size_t i = 0; i < cap; i++)
        if (bs[i] != BS_FILL) last = i + 1;
    char   nals[256];
    size_t nl = 0;
    int    n  = 0;
    nals[0]   = '\0';
    for (size_t i = 0; i + 4 < last && n < 16; i++) {
        if (bs[i] == 0 && bs[i + 1] == 0 && (bs[i + 2] == 1 || (bs[i + 2] == 0 && bs[i + 3] == 1))) {
            size_t const h = i + (bs[i + 2] == 1 ? 3 : 4);
            nl += (size_t)snprintf(nals + nl, sizeof(nals) - nl, "%s[%u,%u]", n ? "," : "", (unsigned)i,
                                   (unsigned)(bs[h] & 0x1f));
            n++;
            i = h;
        }
    }
    char head[3 * 24 + 1];
    for (int i = 0; i < 24; i++) snprintf(head + 3 * i, 4, "%02x ", bs[i]);
    report_emitf("ENCDIAG",
                 "{\"when\":\"bitstream\",\"frame\":%d,\"written_to\":%u,\"code_len\":%lu,\"start_codes\":%d,"
                 "\"nals\":[%s],\"head\":\"%s\"}",
                 frame, (unsigned)last, (unsigned long)code_len, n, nals, head);
    // Everything up to the last written byte, for ffprobe on the PC.
    mkdir(SD_DIR, 0777);
    char path[96];
    snprintf(path, sizeof(path), SD_DIR "/%s_raw%d.h264", run, frame);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(bs, 1, last, f);
        fclose(f);
    }
}

// Encode one frame in a helper task and print every register change for
// up to 11 s (the encoder gives up after 1000 ticks = 10 s at 100 Hz).
static esp_h264_err_t diag_encode(esp_h264_enc_handle_t enc, esp_h264_enc_in_frame_t* in,
                                  esp_h264_enc_out_frame_t* out, bool mask, int frame, char const* run) {
    memset(out->raw_data.buffer, BS_FILL, out->raw_data.len);
    esp_cache_msync(out->raw_data.buffer, out->raw_data.len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    uint32_t max_code_len = 0;
    diag_job_t     job      = {.enc = enc, .in = in, .out = out};
    uint32_t const ena_prev = H264_HW->int_ena.val;
    // Masked, the handler never runs, so whatever the hardware raises
    // stays latched in int_raw where the sampling below sees it.
    if (mask) H264_HW->int_ena.val = 0;
    uint32_t   prev[DIAG_N], cur[DIAG_N];
    diag_sample(prev);
    int64_t const t0 = esp_timer_get_time();
    diag_emit("before", 0, prev);
    xTaskCreatePinnedToCore(diag_task, "enc_diag", 4096, &job, 5, NULL, 1);
    int changes = 0;
    while (!job.done && esp_timer_get_time() - t0 < 11000000) {
        diag_sample(cur);
        if (cur[4] > max_code_len) max_code_len = cur[4];  // frame_code_length, before any reset
        if (memcmp(cur, prev, sizeof(cur)) != 0 && changes < 40) {
            diag_emit("change", esp_timer_get_time() - t0, cur);
            memcpy(prev, cur, sizeof(cur));
            changes++;
        }
        esp_rom_delay_us(200);  // busy: sample far faster than a tick
    }
    while (!job.done) vTaskDelay(1);
    diag_sample(cur);
    if (mask) {
        diag_emit("masked-final", esp_timer_get_time() - t0, cur);
        H264_HW->int_clr.val = 0xffffffff;
        H264_HW->int_ena.val = ena_prev;
    }
    diag_emit(job.err == ESP_H264_ERR_OK ? "done-ok" : "done-error", esp_timer_get_time() - t0, cur);
    diag_bitstream(out->raw_data.buffer, out->raw_data.len, max_code_len, frame, run);
    return job.err;
}

void enctest_run(char const* args, pax_buf_t* fb, void (*blit)(void)) {
    params_t p;
    char     err[128];
    if (!parse(args, &p, err, sizeof(err))) {
        report_emitf("RESULT", "{\"status\":\"error\",\"why\":\"%s\"}", err);
        return;
    }
    int const in_w = pax_buf_get_width_raw(fb);  // 480
    int const in_h = pax_buf_get_height_raw(fb);  // 800
    bool const rotated = p.rot == 90 || p.rot == 270;
    if ((rotated ? in_h : in_w) != OUT_W || (rotated ? in_w : in_h) != OUT_H) {
        report_emitf("RESULT", "{\"status\":\"error\",\"why\":\"framebuffer %dx%d does not give %dx%d at rot %d\"}", in_w,
                     in_h, OUT_W, OUT_H, p.rot);
        return;
    }

    static char json[REPORT_JSON_MAX];
    ledger_t    l_start, l_ppa, l_enc, l_end;
    ledger_take(&l_start);

    // --- set up: PPA client, buffers, encoder ---------------------------
    ppa_client_handle_t       ppa   = NULL;
    esp_h264_enc_handle_t     enc   = NULL;
    uint8_t*                  yuv   = NULL;
    uint8_t*                  bs    = NULL;
    uint32_t*                 times = NULL;  // [ST_COUNT][frames]
    uint32_t*                 sizes = NULL;  // per frame; the top bit marks an I/IDR frame
    FILE*                     f     = NULL;
    char const*               why   = NULL;
    ppa_client_config_t const pcfg  = {
         .oper_type             = PPA_OPERATION_SRM,
         .max_pending_trans_num = 1,
         .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&pcfg, &ppa) != ESP_OK) {
        why = "ppa_register_client";
        goto out;
    }
    ledger_take(&l_ppa);

    yuv   = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);
    bs    = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);  // >= input, or the encoder refuses
    times = heap_caps_calloc((size_t)ST_COUNT * (size_t)p.frames, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    sizes = heap_caps_calloc((size_t)p.frames, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!yuv || !bs || !times || !sizes) {
        why = "no memory for buffers";
        goto out;
    }

    esp_h264_enc_cfg_hw_t const ecfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = (uint8_t)p.gop,
        .fps      = (uint8_t)p.fps,
        .res      = {.width = (uint16_t)p.w, .height = (uint16_t)p.h},
        .rc       = {.bitrate = p.br_kbit * 1000u, .qp_min = (uint8_t)p.qp_min, .qp_max = (uint8_t)p.qp_max},
    };
    if (esp_h264_enc_hw_new(&ecfg, &enc) != ESP_H264_ERR_OK || enc == NULL) {
        why = "esp_h264_enc_hw_new";
        goto out;
    }
    if (esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        why = "esp_h264_enc_open";
        goto out;
    }
    ledger_take(&l_enc);
    if (p.diag) diag_static();

    if (p.save) {
        mkdir(SD_DIR, 0777);
        char path[96];
        snprintf(path, sizeof(path), SD_DIR "/%s.h264", p.run);
        f = fopen(path, "wb");
        if (f == NULL) {
            why = "cannot open the .h264 file on the SD card";
            goto out;
        }
    }

    {
        char a[112], b[112], c[112];
        ledger_json(a, sizeof(a), "start", &l_start);
        ledger_json(b, sizeof(b), "ppa", &l_ppa);
        ledger_json(c, sizeof(c), "enc", &l_enc);
        report_emitf("ENCBEGIN",
                     "{\"run\":\"%s\",\"frames\":%d,\"fps\":%d,\"br\":%lu,\"gop\":%d,\"qp\":[%d,%d],\"pat\":\"%s\","
                     "\"rot\":%d,\"src\":\"%s\",\"res\":[%d,%d],\"pace\":%d,\"show\":%d,\"save\":%d,\"in\":[%d,%d],\"out\":[%d,%d],"
                     "\"enc_sram\":%ld,\"ppa_sram\":%ld,%s,%s,%s}",
                     p.run, p.frames, p.fps, (unsigned long)p.br_kbit, p.gop, p.qp_min, p.qp_max, pattern_name(p.pat),
                     p.rot, p.flat ? "flat" : "ppa", p.w, p.h, p.pace, p.show, p.save, in_w, in_h, OUT_W, OUT_H, (long)l_ppa.sram - (long)l_enc.sram,
                     (long)l_start.sram - (long)l_ppa.sram, a, b, c);
    }

    // --- the frames ---------------------------------------------------------
    size_t const  fb_bytes  = (size_t)in_w * (size_t)in_h * 2;
    int64_t const period_us = 1000000 / p.fps;
    int64_t const t_start   = esp_timer_get_time();
    int64_t       next      = t_start;
    int64_t       t_period  = t_start;
    uint64_t      bytes = 0, bytes_period = 0;
    int           done = 0, frames_period = 0, i_frames = 0, errors = 0;
    uint32_t      enc_max_period = 0;

    for (int n = 0; n < p.frames; n++) {
        if (p.pace) {
            while (esp_timer_get_time() < next) vTaskDelay(1);
            next += period_us;
        }
        uint32_t* const st = times;  // column n of each stage row
        int64_t         t0 = esp_timer_get_time();

        pattern_draw(fb, p.pat, (uint32_t)n, (uint32_t)p.fps);
        int64_t t1 = esp_timer_get_time();

        esp_cache_msync(pax_buf_get_pixels_rw(fb), fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        int64_t t2 = esp_timer_get_time();

        ppa_srm_oper_config_t const srm = {
            .in =
                {
                    .buffer         = pax_buf_get_pixels(fb),
                    .pic_w          = (uint32_t)in_w,
                    .pic_h          = (uint32_t)in_h,
                    .block_w        = (uint32_t)in_w,
                    .block_h        = (uint32_t)in_h,
                    .block_offset_x = 0,
                    .block_offset_y = 0,
                    .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
                },
            .out =
                {
                    .buffer         = yuv,
                    .buffer_size    = YUV_BYTES,
                    .pic_w          = OUT_W,
                    .pic_h          = OUT_H,
                    .block_offset_x = 0,
                    .block_offset_y = 0,
                    .srm_cm         = PPA_SRM_COLOR_MODE_YUV420,
                    .yuv_range      = PPA_COLOR_RANGE_LIMIT,
                    .yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                },
            .rotation_angle = ppa_angle(p.rot),
            .scale_x        = 1.0f,
            .scale_y        = 1.0f,
            .mode           = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t perr = ESP_OK;
        if (p.flat) {
            // Mid-grey luma with a moving bright band, neutral chroma; the
            // CPU writes it, the encoder writes it back from the cache.
            memset(yuv, 128, (size_t)p.w * (size_t)p.h * 3 / 2);
            for (int y = 0; y < p.h; y++) memset(yuv + (size_t)y * p.w * 3 / 2, 235, 16);
            (void)srm;
        } else {
            perr = ppa_do_scale_rotate_mirror(ppa, &srm);
        }
        int64_t         t3   = esp_timer_get_time();

        esp_h264_enc_in_frame_t in = {
            .raw_data = {.buffer = yuv, .len = (uint32_t)(p.w * p.h * 3 / 2)},
            .pts      = (uint32_t)((int64_t)n * 90000 / p.fps),
        };
        esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = bs, .len = (uint32_t)(p.w * p.h * 3 / 2)}};
        esp_h264_err_t const     herr =
            perr != ESP_OK ? ESP_H264_ERR_FAIL : p.diag ? diag_encode(enc, &in, &out, p.diag == 2, n, p.run) : esp_h264_enc_process(enc, &in, &out);
        int64_t t4 = esp_timer_get_time();

        bool const key = out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I;
        if (herr != ESP_H264_ERR_OK) {
            errors++;
        } else {
            if (f && out.length) fwrite(bs, 1, out.length, f);
            bytes += out.length;
            bytes_period += out.length;
            sizes[n] = out.length | (key ? 0x80000000u : 0);
            if (key) i_frames++;
        }
        int64_t t5 = esp_timer_get_time();

        if (n == 0 && p.save) {
            // The source of frame 0, to hold next to the decoded video (1.2).
            char path[96];
            snprintf(path, sizeof(path), SD_DIR "/%s_f0.png", p.run);
            screenshot_capture_to(fb, path);
            t5 = esp_timer_get_time();  // not part of the frame's cost
        }
        if (p.show && blit) blit();
        int64_t t6 = esp_timer_get_time();

        st[ST_DRAW * p.frames + n]  = (uint32_t)(t1 - t0);
        st[ST_SYNC * p.frames + n]  = (uint32_t)(t2 - t1);
        st[ST_PPA * p.frames + n]   = (uint32_t)(t3 - t2);
        st[ST_ENC * p.frames + n]   = (uint32_t)(t4 - t3);
        st[ST_WRITE * p.frames + n] = (uint32_t)(t5 - t4);
        st[ST_SHOW * p.frames + n]  = (uint32_t)(t6 - t5);
        st[ST_TOTAL * p.frames + n] = (uint32_t)(t6 - t0);
        if ((uint32_t)(t4 - t3) > enc_max_period) enc_max_period = (uint32_t)(t4 - t3);
        done++;
        frames_period++;

        int64_t const now = esp_timer_get_time();
        if (now - t_period >= 1000000) {
            double const dt = (double)(now - t_period) / 1e6;
            report_emitf("ENCPERIOD",
                         "{\"n\":%d,\"fps\":%.2f,\"kbit\":%.1f,\"enc_max_us\":%lu,\"errors\":%d,\"sram\":%u}", n,
                         frames_period / dt, (double)bytes_period * 8 / dt / 1000, (unsigned long)enc_max_period,
                         errors, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            t_period       = now;
            frames_period  = 0;
            bytes_period   = 0;
            enc_max_period = 0;
        }
    }
    int64_t const t_end = esp_timer_get_time();

    // --- results --------------------------------------------------------------
    {
        // Frame sizes, I and P apart.
        uint32_t *isz = heap_caps_calloc((size_t)done + 1, sizeof(uint32_t), MALLOC_CAP_SPIRAM),
                 *psz = heap_caps_calloc((size_t)done + 1, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        int ni = 0, np = 0;
        for (int n = 0; n < done && isz && psz; n++) {
            if (sizes[n] & 0x80000000u) {
                isz[ni++] = sizes[n] & 0x7fffffffu;
            } else {
                psz[np++] = sizes[n];
            }
        }
        size_t len = 0;
        len += (size_t)snprintf(json + len, sizeof(json) - len,
                                "{\"run\":\"%s\",\"status\":\"%s\",\"frames\":%d,\"errors\":%d,\"i_frames\":%d,"
                                "\"secs\":%.3f,\"fps\":%.2f,\"bytes\":%llu,\"kbit\":%.1f,\"kbit_at_fps\":%.1f,",
                                p.run, errors ? "bad" : "ok", done, errors, i_frames, (double)(t_end - t_start) / 1e6,
                                done / ((double)(t_end - t_start) / 1e6), (unsigned long long)bytes,
                                (double)bytes * 8 / ((double)(t_end - t_start) / 1e6) / 1000,
                                done ? (double)bytes * 8 * p.fps / done / 1000 : 0.0);
        len += (size_t)snprintf(json + len, sizeof(json) - len, "\"us\":{");
        for (int s = 0; s < ST_COUNT; s++) {
            len += (size_t)stat_json(json + len, sizeof(json) - len, ST_NAMES[s], times + s * p.frames, done);
            len += (size_t)snprintf(json + len, sizeof(json) - len, s + 1 < ST_COUNT ? "," : "},");
        }
        len += (size_t)snprintf(json + len, sizeof(json) - len, "\"frame_bytes\":{");
        len += (size_t)stat_json(json + len, sizeof(json) - len, "i", isz, ni);
        len += (size_t)snprintf(json + len, sizeof(json) - len, ",");
        len += (size_t)stat_json(json + len, sizeof(json) - len, "p", psz, np);
        len += (size_t)snprintf(json + len, sizeof(json) - len, "},");
        heap_caps_free(isz);
        heap_caps_free(psz);
        (void)len;
    }

out:
    if (f) fclose(f);
    if (enc) {
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
    }
    if (ppa) ppa_unregister_client(ppa);
    heap_caps_free(yuv);
    heap_caps_free(bs);
    heap_caps_free(times);
    heap_caps_free(sizes);
    ledger_take(&l_end);

    char a[112], b[112];
    ledger_json(a, sizeof(a), "start", &l_start);
    ledger_json(b, sizeof(b), "end", &l_end);
    if (why) {
        report_emitf("RESULT", "{\"run\":\"%s\",\"status\":\"error\",\"why\":\"%s\",\"ledger\":{%s,%s}}", p.run, why, a,
                     b);
        return;
    }
    size_t const len = strlen(json);
    snprintf(json + len, sizeof(json) - len, "\"leak\":%ld,\"ledger\":{%s,%s}}", (long)l_start.sram - (long)l_end.sram,
             a, b);
    report_emit("RESULT", json);
}
