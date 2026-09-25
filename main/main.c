// =====================================================================
//  nfmtest  --  H.264 screen streaming over USB-NCM, measured
// ---------------------------------------------------------------------
//  See claudeplans/nfmtest.md. What is built so far is the USB network
//  link on its own (steps 1.3, 1.4, 4.1, 4.2 and test T5a): the USB-C
//  port becomes a CDC-NCM adapter, the PC gets 192.168.77.1 by DHCP, the
//  badge is 192.168.77.2, answers ping, and blasts UDP at the PC.
//
//  Started two ways:
//    - from the keyboard (see the screen), for a first look by hand;
//    - by the host over the debug console, `RUN ncm ...`
//      (tools/nfmrun.py), which is hands-free and returns to the
//      launcher afterwards, as the testkit's tests do.
// =====================================================================

#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "driver/gpio.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "esp_log.h"
#include "gl_input.h"
#include "nfm/ncmtest.h"
#include "nfm/usbnet.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "report.h"
#include "testkit/debugcon.h"

static char const TAG[] = "nfmtest";
static char const APP[] = "at.cavac.nfmtest";

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define GREY  0xFF808080
#define BLUE  0xFF2040A0

static size_t                     s_h_res;
static size_t                     s_v_res;
static bsp_display_color_format_t s_color_format;
static bsp_display_endianness_t   s_endian;
static pax_buf_t                  s_fb;
static QueueHandle_t              s_input;
static char const*                s_state = "menu";

static char const* app_state(void) {
    return s_state;
}

static debugcon_identity_t const IDENTITY = {.app = APP, .state = app_state};

static void blit(void) {
    bsp_display_blit(0, 0, s_h_res, s_v_res, pax_buf_get_pixels(&s_fb));
}

static void draw_header(void) {
    pax_background(&s_fb, WHITE);
    pax_simple_rect(&s_fb, BLUE, 0, 0, pax_buf_get_width(&s_fb), 40);
    pax_draw_text(&s_fb, WHITE, pax_font_sky_mono, 24, 10, 8, "nfmtest: USB-C as a network adapter");
}

static void draw_menu(void) {
    uint8_t host[6], dev[6];
    usbnet_macs(host, dev);
    char mac[64];
    snprintf(mac, sizeof(mac), "PC interface: enx%02x%02x%02x%02x%02x%02x", host[0], host[1], host[2], host[3],
             host[4], host[5]);
    draw_header();
    static char const* const lines[] = {
        "Connect the USB-C port to a Linux PC, then:",
        "",
        "  Enter   blast UDP for 30 s (USB ceiling, T5a)",
        "  B       blast UDP for 5 minutes",
        "  I       link only for 120 s: DHCP, ping, echo",
        "  F1      back to the launcher",
        "",
        "The console (/dev/ttyACM0) goes away while the",
        "link is up and comes back afterwards.",
        "",
        "Badge 192.168.77.2, PC 192.168.77.1 (DHCP, no route)",
        "On the PC: tools/nfmrecv.py  (receives :5000/:5001)",
    };
    int y = 60;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++, y += 22)
        pax_draw_text(&s_fb, BLACK, pax_font_sky_mono, 18, 10, y, lines[i]);
    pax_draw_text(&s_fb, GREY, pax_font_sky_mono, 18, 10, y + 10, mac);
    blit();
}

static void hud(char const* const* lines, int n) {
    draw_header();
    for (int i = 0; i < n; i++) pax_draw_text(&s_fb, BLACK, pax_font_sky_mono, 20, 10, 60 + i * 28, lines[i]);
    blit();
}

static void run_ncm(char const* args, bool console) {
    ncmtest_params_t p;
    char             err[96];
    if (!ncmtest_parse(args, &p, err, sizeof(err))) {
        report_emitf("RESULT", "{\"status\":\"error\",\"why\":\"%s\"}", err);
        return;
    }
    s_state = "ncm";
    ncmtest_run(&p, hud, console);
    s_state = "menu";
}

static void leave(void) {
    report_emit("BYE", "{}");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_device_restart_to_launcher();
}

// A command from the host: "RUN ncm k=v ...", "EXIT", "BADGELINK".
static void handle_command(char const* line) {
    if (strncmp(line, "RUN ncm", 7) == 0 && (line[7] == '\0' || line[7] == ' ')) {
        run_ncm(line + 7, true);
        leave();
        return;
    }
    if (strncmp(line, "RUN ", 4) == 0) {
        report_emitf("RESULT", "{\"status\":\"error\",\"why\":\"unknown test: %s\"}", line + 4);
        debugcon_set_busy(false);
        return;
    }
    leave();  // EXIT, BADGELINK
}

static void handle_key(bsp_input_event_t const* ev) {
    char const* args = NULL;
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_F1: leave(); return;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: args = "mode=blast secs=30"; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_KEYBOARD) {
        switch (ev->args_keyboard.ascii) {
            case 'b':
            case 'B': args = "mode=blast secs=300"; break;
            case 'i':
            case 'I': args = "mode=idle secs=120"; break;
            default: break;
        }
    }
    if (args == NULL) return;
    debugcon_set_busy(true);
    run_ncm(args, false);
    debugcon_set_busy(false);
    vTaskDelay(pdMS_TO_TICKS(3000));  // leave the result on screen a moment
    xQueueReset(s_input);             // keys pressed meanwhile do not start another run
    draw_menu();
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    bsp_configuration_t const bsp_configuration = {
        .display = {.requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB, .num_fbs = 1},
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }
    res = bsp_display_get_parameters(&s_h_res, &s_v_res, &s_color_format, &s_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "display parameters: %d", res);
        return;
    }
    pax_buf_type_t const format =
        s_color_format == BSP_DISPLAY_COLOR_FORMAT_24_888RGB ? PAX_BUF_24_888RGB : PAX_BUF_16_565RGB;
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90: orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        default: break;
    }
    pax_buf_init(&s_fb, NULL, s_h_res, s_v_res, format);
    pax_buf_reversed(&s_fb, s_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&s_fb, orientation);

    ESP_ERROR_CHECK(gl_input_get_queue(&s_input));

    debugcon_start(&IDENTITY);
    draw_menu();

    for (;;) {
        char line[DEBUGCON_LINE_MAX];
        if (debugcon_poll(line)) handle_command(line);
        bsp_input_event_t ev;
        if (xQueueReceive(s_input, &ev, pdMS_TO_TICKS(50)) == pdTRUE) handle_key(&ev);
    }
}
