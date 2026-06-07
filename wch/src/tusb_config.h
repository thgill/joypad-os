/*
 * Joypad OS — TinyUSB configuration for CH32V307 usb2usb.
 *
 *   Device output = USBHS (rhport 0, high speed, PB6/PB7)
 *   Host  input   = USBFS (rhport 1, full speed, PA11/PA12)
 *
 * CFG_TUSB_MCU / CFG_TUD_WCH_USBIP_* are supplied by hw/bsp/ch32v30x/family.mk.
 * Device-mode driver gates (CFG_TUD_XID / XINPUT / GC_ADAPTER) and host driver
 * gates (CFG_TUH_XINPUT) mirror the RP2040/ESP32 Joypad config so the shared
 * usbd modes + usbh registry compile unchanged.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------+
// Board / RootHub port assignment
//--------------------------------------------------------------------+

// Device on USBHS = rhport 0 ; Host on USBFS = rhport 1
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT      1
#endif

#ifndef BOARD_TUD_MAX_SPEED
// High-speed: USBHS device only validates at HS on this chip (FS-on-USBHS doesn't
// present on the bus). HS enumeration of the FS-oriented Joypad composite requires
// tud_descriptor_device_qualifier_cb + other-speed-config (see usbd descriptors).
#define BOARD_TUD_MAX_SPEED   OPT_MODE_HIGH_SPEED
#endif
#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED   OPT_MODE_FULL_SPEED
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (set by family.mk)
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED
#define CFG_TUH_ENABLED       1
#define CFG_TUH_MAX_SPEED     BOARD_TUH_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUD_MEM_SECTION
#define CFG_TUD_MEM_SECTION
#endif
#ifndef CFG_TUD_MEM_ALIGN
#define CFG_TUD_MEM_ALIGN         __attribute__ ((aligned(4)))
#endif
#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif
#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN         __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION (USBHS output — Joypad output modes)
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE      64

// Standard HID gamepad mode — up to 4 HID interfaces
#define CFG_TUD_HID                 4
#define CFG_TUD_HID_EP_BUFSIZE      64

// Xbox Original (XID) mode
#define CFG_TUD_XID                 1
#define CFG_TUD_XID_EP_BUFSIZE      32

// Xbox 360 (XInput) mode
#define CFG_TUD_XINPUT              1
#define CFG_TUD_XINPUT_EP_BUFSIZE   32

// GameCube Adapter mode
#define CFG_TUD_GC_ADAPTER          1
#define CFG_TUD_GC_ADAPTER_EP_BUFSIZE 37

// CDC config/telemetry port (joypad-live, SInput config channel)
#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_RX_BUFSIZE      512  // must be >= EP_BUFSIZE or TinyUSB never arms CDC OUT
#define CFG_TUD_CDC_TX_BUFSIZE      512   // trimmed from 1024 for 64KB SRAM
#define CFG_TUD_CDC_EP_BUFSIZE      512  // HS bulk endpoints must be 512 bytes

#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

//--------------------------------------------------------------------
// HOST CONFIGURATION (USBFS input — Joypad usbh registry)
//--------------------------------------------------------------------

#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HUB                 1
#define CFG_TUH_HID                 4
#define CFG_TUH_XINPUT              2   // trimmed from 4 for 64KB SRAM
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 5 : 1)

#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64
#define CFG_TUH_XINPUT_EPIN_BUFSIZE 64

#define CFG_TUH_API_EDPT_XFER       1

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
