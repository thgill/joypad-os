// vmu.h - Dreamcast VMU sub-peripheral emulation for JoypadOS
//
// Implements Dreamcast FT1 (Storage) sub-peripheral emulation.
// VMU image stored in RAM, persisted to SD card as DC_1.VMU.
//
// Maple protocol references:
//   MaplePad by mackieks (CC-BY 4.0) - github.com/mackieks/MaplePad
//   BlueRetro VMU devlog by arrington - blog.arrington.dev
//   dreamcast.wiki/Maple_bus

#pragma once
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// VMU LAYOUT CONSTANTS
// ============================================================================

#define VMU_BLOCK_SIZE          512
#define VMU_PHASE_COUNT_READ    1
#define VMU_PHASE_COUNT_WRITE   4
#define VMU_WRITE_PHASE_SIZE    128
#define VMU_TOTAL_BLOCKS        256
#define VMU_IMAGE_SIZE          (VMU_BLOCK_SIZE * VMU_TOTAL_BLOCKS)  // 131072 bytes

// ============================================================================
// MAPLE ADDRESS
// ============================================================================

#define VMU_SUBPERIPHERAL_ADDR  0x01    // Sub-peripheral slot 0

// ============================================================================
// STATUS
// ============================================================================

typedef enum {
    VMU_STATUS_OK,
    VMU_STATUS_SAVING,
    VMU_STATUS_NO_SD,
    VMU_STATUS_ERROR,
} VmuStatus;

// ============================================================================
// PUBLIC API
// ============================================================================

// Initialize VMU — call from dreamcast_init()
void vmu_init(uint8_t port_addr);
bool vmu_sd_load(void);  // Call after Maple Bus enumeration; returns true if storage backend available

// Volatile activity flag — set by Core 1 on any VMU read/write, cleared by Core 0
extern volatile bool vmu_activity_flag;

// Build pre-built response packets — call from dreamcast_init()
void vmu_build_packets(uint8_t port_addr);

// Packet accessors — called from Core 1 send switches
const void* vmu_get_device_info_packet(uint32_t *size_words);
const void* vmu_get_all_device_info_packet(uint32_t *size_words);
const void* vmu_get_media_info_packet(uint32_t *size_words);
const void* vmu_get_block_read_packet(uint32_t *size_words);
const void* vmu_get_ack_packet(uint32_t *size_words);

// Block read/write handlers — called from ConsumePacket on Core 1
const void* vmu_handle_block_read(const uint32_t *packet_data, uint32_t *size_words);
void        vmu_handle_block_write(const uint32_t *packet_data, uint32_t num_words);
void        vmu_handle_lcd_write(const uint8_t* data, uint32_t len);
void        vmu_handle_write_complete(void);

// Address and status
uint8_t   vmu_get_address(void);
VmuStatus vmu_get_status(void);

// Slot management (1-4 for flash slots)
void    vmu_set_slot(uint8_t slot);
uint8_t vmu_get_slot(void);

// Core 0 periodic task — processes pending flash writes
// MUST be called from Core 0 (flash writes require Core 0)
void vmu_task(void);
