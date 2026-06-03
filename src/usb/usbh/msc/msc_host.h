// msc_host.h — TinyUSB Mass Storage Class host glue
//
// Goal: detect a USB flash drive plugged into the same hub as the
// controller, expose a synchronous block-IO interface so FatFs can
// mount it, and let vmu_storage advertise the VMU only when a drive
// is present.
//
// API is intentionally small — single-LUN, read/write 512-byte blocks,
// blocking with a timeout (TinyUSB MSC API is async; we spin on
// tuh_task() until completion).

#ifndef MSC_HOST_H
#define MSC_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void msc_host_init(void);

// True after a flash drive has fully enumerated AND we've cached its
// capacity. Polled by vmu_storage at boot (with a wait-for-enum window).
bool msc_host_mounted(void);

// Block geometry. Valid only when mounted.
uint32_t msc_host_block_count(void);
uint32_t msc_host_block_size(void);

// Synchronous block IO. Returns true on success. Internally pumps
// tuh_task() until the async completion callback fires or timeout.
bool msc_host_read_blocks(uint32_t lba, void* buf, uint32_t count);
bool msc_host_write_blocks(uint32_t lba, const void* buf, uint32_t count);

#endif // MSC_HOST_H
