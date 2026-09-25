#pragma once
// =====================================================================
//  TinyUSB configuration for nfmtest (hand-written, F-08)
// ---------------------------------------------------------------------
//  A grace app compiles against graceloader's sdkconfig.h, which has no
//  CONFIG_TINYUSB_*, so esp_tinyusb's generated tusb_config.h cannot be
//  used. This one is what that file would produce for:
//
//    - device only, full speed, on rhport 0 (the OTG 1.1 controller
//      behind the USB-C port's FSLS PHY, F-01);
//    - one class: CDC-NCM;
//    - DWC2 in slave (FIFO) mode (D-12): the app's .bss lives in PSRAM
//      (F-09), and slave mode never lets the controller DMA into it.
// =====================================================================

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU    OPT_MCU_ESP32P4
#define CFG_TUSB_OS     OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG  0

// FreeRTOS headers are reached as "freertos/..." on ESP-IDF.
#define CFG_TUSB_OS_INC_PATH freertos/

#define CFG_TUD_ENABLED   1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUH_ENABLED   0

// Slave (FIFO) mode only, no DMA (D-12).
#define CFG_TUD_DWC2_SLAVE_ENABLE 1
#define CFG_TUD_DWC2_DMA_ENABLE   0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64

// Classes: NCM only.
#define CFG_TUD_CDC       0
#define CFG_TUD_MSC       0
#define CFG_TUD_HID       0
#define CFG_TUD_MIDI      0
#define CFG_TUD_VENDOR    0
#define CFG_TUD_ECM_RNDIS 0
#define CFG_TUD_NCM       1
#define CFG_TUD_DFU       0
#define CFG_TUD_DFU_RUNTIME 0
#define CFG_TUD_BTH       0

// NCM transfer blocks.
//
// IN (badge -> PC) is the stream direction. An IN NTB of 8 KB holds five
// 1 316-byte stream datagrams (1 358 B as Ethernet frames), so the
// per-transfer overhead is paid once per five datagrams; three of them
// let the application fill one while one is on the wire. Linux's
// cdc_ncm takes up to 32 KB per IN NTB (it asks with SET_NTB_INPUT_SIZE
// and the driver honours it), so 8 KB is well inside what it accepts.
//
// OUT (PC -> badge) carries only ARP, ICMP and the odd control datagram,
// so the defaults are enough.
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE           8192
#define CFG_TUD_NCM_IN_NTB_N                  3
#define CFG_TUD_NCM_IN_MAX_DATAGRAMS_PER_NTB  8
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE          3200
#define CFG_TUD_NCM_OUT_NTB_N                 2
#define CFG_TUD_NCM_OUT_MAX_DATAGRAMS_PER_NTB 6

#ifdef __cplusplus
}
#endif
