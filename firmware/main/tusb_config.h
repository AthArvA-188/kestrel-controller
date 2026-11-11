// TinyUSB configuration for Kestrel. The device has one HID interface in
// DInput mode and one vendor interface, handled by the driver in usb_dev.c,
// in XInput mode.
#pragma once

#include "sdkconfig.h"

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_ESP32S3
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_FREERTOS
#endif
#ifndef CFG_TUSB_OS_INC_PATH
#define CFG_TUSB_OS_INC_PATH freertos/
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_HID 1
#define CFG_TUD_HID_EP_BUFSIZE 64
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
