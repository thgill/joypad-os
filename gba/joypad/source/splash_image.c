// splash_image.c — implementation of the bitmap-blit splash renderer.

#include <string.h>
#include <stdbool.h>
#include <gba_video.h>
#include "splash_image.h"

// Per-mode image declarations — defined in splash_images/img_<mode>.c.
// Add a new declaration here AND an entry in splash_image_for() to
// wire a new image in. Order matches joypad_mode_id_t for readability,
// not enforcement.
#define DECL_IMG(sym) extern const splash_image_t sym
DECL_IMG(splash_img_switch);
DECL_IMG(splash_img_xinput);
DECL_IMG(splash_img_ps3);
DECL_IMG(splash_img_ps4);
DECL_IMG(splash_img_sinput);
DECL_IMG(splash_img_keyboard_mouse);
DECL_IMG(splash_img_gc_adapter);
DECL_IMG(splash_img_xbox_original);
DECL_IMG(splash_img_xbone);
DECL_IMG(splash_img_generic);
#undef DECL_IMG

// Each img_<mode>.c declares `const splash_image_t splash_img_*` as a
// strong global. If the .c isn't compiled (e.g. nobody dropped a PNG
// for that mode), the link will fail — so wrap with a weak fallback
// definition here that returns NULL. We use __attribute__((weak)) on
// pointer accessors instead to avoid that complication: the lookup
// table is built with #if defined() against compile-time flags set
// by the Makefile when a PNG exists.
//
// For now the Makefile compiles every splash_images/*.c it finds, so
// only declare images that actually have asset files committed.

// Compile-time presence: if the .c file exists, the linker resolves
// the extern. Otherwise we get an undefined-reference error at link
// time. To avoid that for "image not yet authored" modes, this table
// only references images we actually have on disk. Edit alongside
// committing a new splash_images/img_<mode>.c file.
const splash_image_t* splash_image_for(joypad_mode_id_t mode)
{
    switch (mode) {
#ifdef HAVE_SPLASH_SWITCH
        case JOYPAD_MODE_SWITCH:          return &splash_img_switch;
#endif
#ifdef HAVE_SPLASH_XINPUT
        case JOYPAD_MODE_XINPUT:          return &splash_img_xinput;
#endif
#ifdef HAVE_SPLASH_PS3
        case JOYPAD_MODE_PS3:             return &splash_img_ps3;
#endif
#ifdef HAVE_SPLASH_PS4
        case JOYPAD_MODE_PS4:             return &splash_img_ps4;
#endif
#ifdef HAVE_SPLASH_SINPUT
        case JOYPAD_MODE_SINPUT:          return &splash_img_sinput;
#endif
#ifdef HAVE_SPLASH_KEYBOARD_MOUSE
        case JOYPAD_MODE_KEYBOARD_MOUSE:  return &splash_img_keyboard_mouse;
#endif
#ifdef HAVE_SPLASH_GC_ADAPTER
        case JOYPAD_MODE_GC_ADAPTER:      return &splash_img_gc_adapter;
#endif
#ifdef HAVE_SPLASH_XBOX_ORIGINAL
        case JOYPAD_MODE_XBOX_ORIGINAL:   return &splash_img_xbox_original;
#endif
#ifdef HAVE_SPLASH_XBONE
        case JOYPAD_MODE_XBONE:           return &splash_img_xbone;
#endif
        default: return NULL;
    }
}

const splash_image_t* splash_image_fallback(void)
{
#ifdef HAVE_SPLASH_GENERIC
    return &splash_img_generic;
#elif defined(HAVE_SPLASH_SINPUT)
    // No dedicated generic asset yet — fall back to the SInput image
    // so the user still sees a real splash, just with their mode name
    // overlaid on top. Replace this branch by dropping a
    // generic.png into gba/joypad/assets/ and re-running the
    // png_to_splash.py converter.
    return &splash_img_sinput;
#else
    return (const splash_image_t*)0;
#endif
}

// ============================================================================
// Mode-name lookup for the text overlay. Strings are uppercase and short
// so they fit on one centered line. Unknown modes render as "MODE N".
// ============================================================================
const char* splash_image_mode_name(joypad_mode_id_t mode)
{
    static char unknown[10];
    switch (mode) {
        case JOYPAD_MODE_HID:            return "HID";
        case JOYPAD_MODE_SINPUT:         return "SINPUT";
        case JOYPAD_MODE_XINPUT:         return "XINPUT";
        case JOYPAD_MODE_PS3:            return "PS3";
        case JOYPAD_MODE_PS4:            return "PS4";
        case JOYPAD_MODE_SWITCH:         return "SWITCH";
        case JOYPAD_MODE_KEYBOARD_MOUSE: return "KB MOUSE";
        case JOYPAD_MODE_XBONE:          return "XBOX ONE";
        case JOYPAD_MODE_XBOX_ORIGINAL:  return "XBOX OG";
        case JOYPAD_MODE_GC_ADAPTER:     return "GC ADAPTER";
        case JOYPAD_MODE_CDC:            return "CDC";
        case JOYPAD_MODE_PSCLASSIC:      return "PS CLASSIC";
        case JOYPAD_MODE_PCEMINI:        return "PCE MINI";
        case JOYPAD_MODE_GBA_LINK:       return "GBA LINK";
        default: {
            int m = (int)mode;
            unknown[0] = 'M'; unknown[1] = 'O'; unknown[2] = 'D';
            unknown[3] = 'E'; unknown[4] = ' ';
            if (m >= 100) {
                unknown[5] = '0' + (m / 100); m %= 100;
                unknown[6] = '0' + (m / 10);
                unknown[7] = '0' + (m % 10);
                unknown[8] = '\0';
            } else if (m >= 10) {
                unknown[5] = '0' + (m / 10);
                unknown[6] = '0' + (m % 10);
                unknown[7] = '\0';
            } else {
                unknown[5] = '0' + m;
                unknown[6] = '\0';
            }
            return unknown;
        }
    }
}

// ============================================================================
// Text overlay — 5x7 ASCII font. Writes glyph pixels into the Mode-4
// 8bpp framebuffer (240x160) using palette index 255 = white (the
// converter reserves this slot; see png_to_splash.py). VRAM in Mode 4
// only supports 16-bit writes, so each pixel is a read-modify-write
// halfword.
// ============================================================================
static const uint8_t font5x7[][7] = {
    {0,0,0,0,0,0,0},                            // ' '
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // '!' '"'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // '#' '$'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // '%' '&'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // ''' '('
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // ')' '*'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},       // '+'
    {0,0,0,0,0,0,0},                            // ','
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},       // '-'
    {0,0,0,0,0,0,0},                            // '.'
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10},       // '/'
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},       // '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},       // '1'
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},       // '2'
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},       // '3'
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},       // '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},       // '5'
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},       // '6'
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},       // '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},       // '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},       // '9'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // ':' ';'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // '<' '='
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},           // '>' '?'
    {0,0,0,0,0,0,0},                            // '@'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},       // 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},       // 'B'
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},       // 'C'
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},       // 'D'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},       // 'E'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},       // 'F'
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},       // 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},       // 'H'
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},       // 'I'
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},       // 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},       // 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},       // 'L'
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},       // 'M'
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11},       // 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},       // 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},       // 'P'
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},       // 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},       // 'R'
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},       // 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},       // 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},       // 'U'
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},       // 'V'
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},       // 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},       // 'X'
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},       // 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},       // 'Z'
};

// Write a single pixel into Mode-4 page-0 VRAM at palette index 255.
// Also paints a 1-pixel black drop-shadow at (x+1, y+1) using palette
// index 0 so the white text stays legible over light image regions.
static inline void overlay_pixel(int x, int y)
{
    if (x < 0 || x >= 240 || y < 0 || y >= 160) return;
    uint16_t* vram = (uint16_t*)0x06000000;
    int hw = (y * 120) + (x >> 1);
    uint16_t cur = vram[hw];
    if (x & 1) cur = (cur & 0x00FF) | (0xFF << 8);
    else       cur = (cur & 0xFF00) | 0xFF;
    vram[hw] = cur;
}

static void overlay_glyph(int x, int y, char c)
{
    if (c < ' ' || c > 'Z') c = ' ';
    const uint8_t* g = font5x7[c - ' '];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                overlay_pixel(x + col, y + row);
            }
        }
    }
}

void splash_image_overlay_text(int cx, int y, const char* text)
{
    if (!text) return;
    int len = 0;
    for (const char* p = text; *p; p++) len++;
    if (len == 0) return;
    // Scale 2x (10x14 per glyph + 2 px spacing) so the label reads
    // clearly across a 240-wide screen. Walk the bits and write a 2x2
    // block per set pixel.
    const int GLYPH_W = 5 * 2 + 2;   // 10 px + 2 px tracking
    int width = len * GLYPH_W - 2;
    int x = cx - width / 2;
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c < ' ' || c > 'Z') c = ' ';
        const uint8_t* g = font5x7[c - ' '];
        for (int row = 0; row < 7; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < 5; col++) {
                if (bits & (0x10 >> col)) {
                    int px = x + col * 2;
                    int py = y + row * 2;
                    overlay_pixel(px,     py);
                    overlay_pixel(px + 1, py);
                    overlay_pixel(px,     py + 1);
                    overlay_pixel(px + 1, py + 1);
                }
            }
        }
        x += GLYPH_W;
    }
    (void)overlay_glyph;  // 1x helper kept for future use
}

void splash_image_render(const splash_image_t* img)
{
    if (!img) return;

    const bool indexed = (img->palette != NULL && img->pixels8 != NULL);
    const bool direct  = (img->pixels  != NULL);
    if (!indexed && !direct) return;

    int img_w = img->width, img_h = img->height;
    int x_off = (240 - img_w) / 2;
    int y_off = (160 - img_h) / 2;
    if (x_off < 0) x_off = 0;
    if (y_off < 0) y_off = 0;
    int copy_w = img_w; if (copy_w + x_off > 240) copy_w = 240 - x_off;
    int copy_h = img_h; if (copy_h + y_off > 160) copy_h = 160 - y_off;

    if (indexed) {
        // Mode-4: 240x160 8bpp indexed, double-page. Use page 0.
        REG_DISPCNT = 0x0004 | 0x0400;  // mode 4 + BG2 enable, front=page0

        // Load palette (256 entries × 2 bytes) → BG palette base.
        uint16_t* pal = (uint16_t*)0x05000000;
        for (int i = 0; i < 256; i++) pal[i] = img->palette[i];

        // VRAM in Mode 4 writes must be at least 16-bit. We pack pixel
        // pairs and write halfwords. Source 8bpp blit at x_off, y_off.
        uint16_t* vram = (uint16_t*)VRAM;
        const uint16_t bg_pair = ((uint16_t)0 << 8) | 0;  // index 0
        const int row_halfwords = 240 / 2;

        // Fill entire framebuffer with index 0 (which palette[0] should
        // be set to the desired bg_color by the converter).
        for (int i = 0; i < 240 * 160 / 2; i++) vram[i] = bg_pair;

        // Blit indexed image. To handle odd x_off, do byte-level via
        // 16-bit halfword reads of two src bytes at a time, but VRAM
        // forbids 8-bit writes — so read-modify-write halfwords.
        for (int y = 0; y < copy_h; y++) {
            const uint8_t* src = img->pixels8 + (int)y * img_w;
            int dst_y = y + y_off;
            int dst_x = x_off;
            for (int x = 0; x < copy_w; x++) {
                int vx = dst_x + x;
                int hw_idx = (dst_y * row_halfwords) + (vx >> 1);
                uint16_t cur = vram[hw_idx];
                if (vx & 1) {
                    cur = (cur & 0x00FF) | ((uint16_t)src[x] << 8);
                } else {
                    cur = (cur & 0xFF00) | (uint16_t)src[x];
                }
                vram[hw_idx] = cur;
            }
        }
        return;
    }

    // Direct-color (Mode-3) path — kept for backwards compatibility
    // with images that were converted before the 8bpp pipeline.
    REG_DISPCNT = 0x0003 | 0x0400;
    uint16_t* vram = (uint16_t*)VRAM;
    uint32_t bg32 = ((uint32_t)img->bg_color << 16) | img->bg_color;
    uint32_t* vram32 = (uint32_t*)vram;
    for (uint32_t i = 0; i < 240 * 160 / 2; i++) vram32[i] = bg32;
    for (int y = 0; y < copy_h; y++) {
        const uint16_t* src = img->pixels + (int)y * img_w;
        uint16_t* dst = vram + ((y + y_off) * 240) + x_off;
        for (int x = 0; x < copy_w; x++) dst[x] = src[x];
    }
}
