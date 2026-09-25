# TinyUSB, vendored subset

- **Upstream:** https://github.com/hathach/tinyusb (MIT, `LICENSE`)
- **Version:** 0.21.0, as the Espressif component `espressif/tinyusb` 0.21.0~1
- **Copied from:** `tanmatsu-launcher/managed_components/espressif__tinyusb/`
  (the copy the launcher's USB device mode runs), 2026-09-25
- **What:** `src/tusb.{c,h}`, `src/tusb_option.h`, `src/common/`, `src/device/`,
  `src/osal/osal.h` + `osal_freertos.h`, `src/class/net/` (NCM only:
  `ncm_device.c`, `ncm.h`, `net_device.h`), `src/class/cdc/cdc.h`, and the DWC2 port
  (`dcd_dwc2.c`, `dwc2_common.{c,h}`, `dwc2_type.h`, `dwc2_esp32.h`).
- **Changes:** none. Configuration is `components/tusb_config.h` (hand-written,
  F-08 in `claudeplans/nfmtest.md`); the build is in the top-level `CMakeLists.txt`.
