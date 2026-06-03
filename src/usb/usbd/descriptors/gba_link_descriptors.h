// gba_link_descriptors.h - GBA Link Cable bridge USB descriptors (vendor class)
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Robert Dale Smith
//
// Custom USB Vendor-class device for bridging GameCube↔GBA link cable
// traffic between a forked Dolphin emulator and a real GBA. Replaces the
// gc2eth_feather TCP-over-Ethernet path with native USB bulk endpoints —
// targets ~125-500µs per-cmd round-trip vs Ethernet's ~1.5-2.5ms,
// enough to make sustained-traffic games (FFCC, Crystal Chronicles,
// Pac-Man Vs.) actually playable.
//
// Wire protocol is the SAME raw joybus byte stream the existing TCP
// path uses — Dolphin sends 1-byte cmd (or 5-byte WRITE), bridge
// replies with 3 / 5 / 1 bytes per the joybus command type. USB just
// becomes a faster transport for the same bytes.
//
// Composite device:
//   IF 0+1: CDC ACM        (debug + remote BOOTSEL, same as today)
//   IF 2:   Vendor (bulk)  (GBA-link traffic to/from Dolphin)
//
// VID/PID rationale: We already impersonate the official Nintendo
// GameCube Adapter (VID 057E, PID 0337) for USB_OUTPUT_MODE_GC_ADAPTER.
// PID 0338 is adjacent to that, has no known assignment, and lets the
// Dolphin fork find us by exact match without false positives against
// other Nintendo devices.

#ifndef GBA_LINK_DESCRIPTORS_H
#define GBA_LINK_DESCRIPTORS_H

#include <stdint.h>
#include "tusb.h"

// ============================================================================
// USB IDENTIFIERS
// ============================================================================
#define GBA_LINK_VID            0x057E  // Nintendo
#define GBA_LINK_PID            0x0338  // (unused; adjacent to GC Adapter 0337)
#define GBA_LINK_BCD_DEVICE     0x0100  // v1.0

// Bulk endpoint sizes. Joybus replies max out at 5 bytes (READ); cmds
// max out at 5 bytes (WRITE = cmd + 4 payload). Sized at 64 to match
// USB FS bulk max-packet so every cmd/reply is single-packet.
#define GBA_LINK_EP_SIZE        64

#define GBA_LINK_MANUFACTURER   "Nintendo"
#define GBA_LINK_PRODUCT        "GameCube GBA Link Cable"

// Interface numbers (composite layout: CDC takes 2, Vendor takes 1)
#define GBA_LINK_ITF_CDC_CTRL   0
#define GBA_LINK_ITF_CDC_DATA   1
#define GBA_LINK_ITF_VENDOR     2
#define GBA_LINK_ITF_TOTAL      3

// Endpoint addresses. CDC uses 0x81 (IN notify), 0x82/0x02 (data IN/OUT).
// Vendor takes 0x83 / 0x03 — distinct from CDC and from any HID
// endpoint a future composite mode might add.
#define GBA_LINK_EP_CDC_NOTIF   0x81
#define GBA_LINK_EP_CDC_OUT     0x02
#define GBA_LINK_EP_CDC_IN      0x82
#define GBA_LINK_EP_VENDOR_OUT  0x03
#define GBA_LINK_EP_VENDOR_IN   0x83

// ============================================================================
// DEVICE DESCRIPTOR
// ============================================================================
static const tusb_desc_device_t gba_link_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,        // composite via IAD
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = GBA_LINK_VID,
    .idProduct          = GBA_LINK_PID,
    .bcdDevice          = GBA_LINK_BCD_DEVICE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// ============================================================================
// CONFIGURATION DESCRIPTOR
// ============================================================================
// Total: Config(9) + CDC(8 IAD + 9 itf + 5+5+4+5 func + 7 ep + 9 itf + 7 ep + 7 ep)
//        + Vendor(9 itf + 7 ep + 7 ep)
//      = 9 + 66 + 23 = 98
#define GBA_LINK_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN \
                                    + TUD_CDC_DESC_LEN \
                                    + TUD_VENDOR_DESC_LEN)

static const uint8_t gba_link_config_descriptor[] = {
    // Config: 1 config, N interfaces, no string idx, total len, attr, 500mA
    TUD_CONFIG_DESCRIPTOR(1, GBA_LINK_ITF_TOTAL, 0,
                          GBA_LINK_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // CDC ACM (debug port + remote BOOTSEL trigger via 'B' character).
    // Args: itf, str_idx, ep_notif_addr, ep_notif_size, ep_out, ep_in, ep_size
    TUD_CDC_DESCRIPTOR(GBA_LINK_ITF_CDC_CTRL, 0,
                       GBA_LINK_EP_CDC_NOTIF, 8,
                       GBA_LINK_EP_CDC_OUT, GBA_LINK_EP_CDC_IN, 64),

    // Vendor interface (GBA-link bulk pipe to Dolphin).
    // Args: itf, str_idx, ep_out, ep_in, ep_size
    TUD_VENDOR_DESCRIPTOR(GBA_LINK_ITF_VENDOR, 0,
                          GBA_LINK_EP_VENDOR_OUT, GBA_LINK_EP_VENDOR_IN,
                          GBA_LINK_EP_SIZE),
};

#endif // GBA_LINK_DESCRIPTORS_H
