// splash.c — boot splash for the gc2usb GBA payload. Draws a
// recognizable per-USB-output-mode badge using the same display.c
// primitives as the eye animation. All shapes are vector — no
// embedded bitmaps — so the .gba size cost is small.
//
// Color contract:
//   * BG  is always black (display_clear sets palette index 0).
//   * FG  ("eye white") is the mode's primary color, set by the
//     caller via display_set_fg_color() BEFORE calling splash_render.
//   * PUPIL is the mode's accent (text strokes, secondary shape).
//
// Layout — everything is anchored to the 240×128 logical surface
// (DISPLAY_WIDTH × DISPLAY_HEIGHT). Vertical center is y=64.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "display.h"
#include "splash.h"

#define RGB5(r,g,b) (uint16_t)((r) | ((g) << 5) | ((b) << 10))

// ============================================================================
// Tiny 5x7 ASCII font — covers '0'-'9', 'A'-'Z', '+', '-', ' '.
// Each glyph is 5 bits per row, 7 rows. MSB is leftmost pixel.
// Designed for legibility at the 1:1 (no upscale) rendering scale.
// ============================================================================
static const uint8_t font5x7[][7] = {
    // Index aligned to ASCII offset from ' ' (space = 0)
    {0,0,0,0,0,0,0},                     // ' '
    {0,0,0,0,0,0,0},                     // '!' (unused, placeholder)
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '"' '#'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '$' '%'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '&' '''
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '(' ')'
    {0,0,0,0,0,0,0},                     // '*'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},// '+'
    {0,0,0,0,0,0,0},                     // ','
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},// '-'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '.' '/'
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},// '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},// '1'
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},// '2'
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},// '3'
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},// '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},// '5'
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},// '6'
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},// '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},// '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},// '9'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // ':' ';'
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '<' '='
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},    // '>' '?'
    {0,0,0,0,0,0,0},                     // '@'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},// 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},// 'B'
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},// 'C'
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},// 'D'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},// 'E'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},// 'F'
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},// 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},// 'H'
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},// 'I'
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},// 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},// 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},// 'L'
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},// 'M'
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11},// 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},// 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},// 'P'
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},// 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},// 'R'
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},// 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},// 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},// 'U'
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},// 'V'
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},// 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},// 'X'
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},// 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},// 'Z'
};

static void draw_glyph(int x, int y, char c, bool pupil_color)
{
    if (c < ' ' || c > 'Z') c = ' ';
    const uint8_t* g = font5x7[c - ' '];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                if (pupil_color) display_pupil_pixel(x + col, y + row);
                else             display_pixel(x + col, y + row, true);
            }
        }
    }
}

// Render a NUL-terminated string centered on cx, top at y.
// Char width = 5 + 1 spacing = 6. Returns the X start used.
static int draw_text_centered(int cx, int y, const char* s, bool pupil)
{
    int len = 0;
    for (const char* p = s; *p; p++) len++;
    int width = len * 6 - 1;  // 5 + 1 spacing, minus trailing space
    int x = cx - width / 2;
    int x0 = x;
    while (*s) {
        draw_glyph(x, y, *s, pupil);
        x += 6;
        s++;
    }
    return x0;
}

// Draw an outline rectangle (1 px thick).
static void draw_rect_outline(int x, int y, int w, int h, bool fg, bool pupil)
{
    if (pupil) {
        display_hspan_pupil(x, y, w);
        display_hspan_pupil(x, y + h - 1, w);
        for (int yy = y + 1; yy < y + h - 1; yy++) {
            display_pupil_pixel(x, yy);
            display_pupil_pixel(x + w - 1, yy);
        }
    } else {
        display_hspan(x, y, w, fg);
        display_hspan(x, y + h - 1, w, fg);
        for (int yy = y + 1; yy < y + h - 1; yy++) {
            display_pixel(x, yy, fg);
            display_pixel(x + w - 1, yy, fg);
        }
    }
}

// Filled circle (Bresenham midpoint) with FG fill.
static void fill_circle(int cx, int cy, int r, bool pupil)
{
    int x = r, y = 0, err = 0;
    while (x >= y) {
        int span = 2 * x + 1;
        if (pupil) {
            display_hspan_pupil(cx - x, cy + y, span);
            display_hspan_pupil(cx - x, cy - y, span);
            display_hspan_pupil(cx - y, cy + x, 2 * y + 1);
            display_hspan_pupil(cx - y, cy - x, 2 * y + 1);
        } else {
            display_hspan(cx - x, cy + y, span, true);
            display_hspan(cx - x, cy - y, span, true);
            display_hspan(cx - y, cy + x, 2 * y + 1, true);
            display_hspan(cx - y, cy - x, 2 * y + 1, true);
        }
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err > 0)  { x--; err -= 2 * x + 1; }
    }
}

// Outline circle, 1 px thick.
static void circle_outline(int cx, int cy, int r, bool pupil)
{
    int x = r, y = 0, err = 0;
    while (x >= y) {
        int pts[8][2] = {
            { cx + x, cy + y }, { cx + y, cy + x },
            { cx - y, cy + x }, { cx - x, cy + y },
            { cx - x, cy - y }, { cx - y, cy - x },
            { cx + y, cy - x }, { cx + x, cy - y },
        };
        for (int i = 0; i < 8; i++) {
            if (pupil) display_pupil_pixel(pts[i][0], pts[i][1]);
            else       display_pixel(pts[i][0], pts[i][1], true);
        }
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err > 0)  { x--; err -= 2 * x + 1; }
    }
}

// Filled triangle (top-down scanline). Used for the PlayStation △.
static void fill_triangle_pointing_up(int cx, int top_y, int half_base,
                                      int height, bool pupil)
{
    for (int row = 0; row < height; row++) {
        int half_w = (half_base * row) / height;
        if (pupil) display_hspan_pupil(cx - half_w, top_y + row, half_w * 2 + 1);
        else       display_hspan(cx - half_w, top_y + row, half_w * 2 + 1, true);
    }
}

// Draw an X (two crossing diagonal lines, 1 px thick).
static void draw_x_mark(int cx, int cy, int radius, bool pupil)
{
    for (int i = -radius; i <= radius; i++) {
        if (pupil) {
            display_pupil_pixel(cx + i, cy + i);
            display_pupil_pixel(cx + i, cy - i);
        } else {
            display_pixel(cx + i, cy + i, true);
            display_pixel(cx + i, cy - i, true);
        }
    }
}

// Filled rectangle.
static void fill_rect(int x, int y, int w, int h, bool pupil)
{
    for (int yy = y; yy < y + h; yy++) {
        if (pupil) display_hspan_pupil(x, yy, w);
        else       display_hspan(x, yy, w, true);
    }
}

// ============================================================================
// Per-mode splashes — Each draws a recognizable badge centered on the
// 240×128 surface, plus a text label below. Color theme is set via
// splash_palette() before splash_render(); badges use FG + pupil color.
// ============================================================================

static void splash_xinput(void)
{
    // Xbox 360 green sphere — big FG circle, small "X" in pupil color
    // through the middle. Label "XINPUT" below.
    const int cx = 120, cy = 50;
    fill_circle(cx, cy, 28, false);
    draw_x_mark(cx, cy, 14, true);
    draw_text_centered(120, 100, "XINPUT", false);
}

static void splash_ps4(void)
{
    // PlayStation symbol diamond: △ □ ○ ✕
    const int cx = 120, cy = 50;
    const int r  = 10;
    // △ top
    fill_triangle_pointing_up(cx, cy - 26, 10, 14, false);
    // ○ right
    circle_outline(cx + 28, cy, r, false);
    circle_outline(cx + 28, cy, r - 1, false);
    // ✕ bottom
    draw_x_mark(cx, cy + 26, 9, false);
    // □ left
    draw_rect_outline(cx - 38, cy - 9, 19, 19, true, false);
    draw_rect_outline(cx - 37, cy - 8, 17, 17, true, false);
    draw_text_centered(120, 100, "PS4", false);
}

static void splash_ps3(void)
{
    splash_ps4();
    // Overwrite label area with PS3 (cheap reuse — diamond is the same)
    // Clear text region first by drawing BG hspans then re-draw
    for (int y = 100; y < 107; y++) display_hspan(80, y, 80, false);
    draw_text_centered(120, 100, "PS3", false);
}

static void splash_switch(void)
{
    // Nintendo Switch — two joycon halves: left half FG-color rectangle
    // with circle (analog stick), right half pupil-color rectangle with
    // four small circles (face buttons).
    const int top = 26, h = 44;
    // Left joycon (FG)
    fill_rect(60, top, 50, h, false);
    // Cut a circular notch (analog stick) in BG
    for (int y = -8; y <= 8; y++) {
        for (int x = -8; x <= 8; x++) {
            if (x*x + y*y <= 64) {
                display_pixel(85 + x, top + 22 + y, false);
            }
        }
    }
    // Right joycon (pupil)
    fill_rect(130, top, 50, h, true);
    // Four small face-button circles cut into BG
    int positions[4][2] = { {155, 36}, {165, 48}, {155, 60}, {145, 48} };
    for (int i = 0; i < 4; i++) {
        int px = positions[i][0], py = positions[i][1];
        for (int y = -4; y <= 4; y++) {
            for (int x = -4; x <= 4; x++) {
                if (x*x + y*y <= 16) {
                    display_pixel(px + x, py + y, false);
                }
            }
        }
    }
    draw_text_centered(120, 100, "SWITCH", false);
}

static void splash_sinput(void)
{
    // SInput — large stylized "S" centered.
    const int cx = 120, cy = 50;
    // S shape via three horizontal bars + two short vertical connectors
    fill_rect(cx - 24, cy - 24, 48, 8, false);  // top
    fill_rect(cx - 24, cy - 4,  48, 8, false);  // middle
    fill_rect(cx - 24, cy + 16, 48, 8, false);  // bottom
    fill_rect(cx - 24, cy - 24, 8,  20, false); // top-left vertical
    fill_rect(cx + 16, cy - 4,  8,  20, false); // bottom-right vertical
    draw_text_centered(120, 100, "SINPUT", false);
}

static void splash_keyboard_mouse(void)
{
    // Keyboard outline + small mouse to the right
    draw_rect_outline(40, 30, 120, 44, false, false);
    draw_rect_outline(41, 31, 118, 42, false, false);
    // Three rows of "keys" as small pupil rects
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 10; col++) {
            int kx = 48 + col * 11;
            int ky = 38 + row * 12;
            fill_rect(kx, ky, 8, 8, true);
        }
    }
    // Mouse: oval shape on right
    fill_circle(190, 50, 14, false);
    display_pixel(190, 38, false);  // visible separator (line down middle)
    for (int y = 38; y <= 50; y++) display_pixel(190, y, false);
    draw_text_centered(120, 100, "KB MOUSE", false);
}

static void splash_xbone(void)
{
    // Xbox One — same green-sphere-with-X as XInput, label says XBOX ONE
    const int cx = 120, cy = 50;
    fill_circle(cx, cy, 28, false);
    draw_x_mark(cx, cy, 14, true);
    draw_text_centered(120, 100, "XBOX ONE", false);
}

static void splash_xbox_og(void)
{
    // Xbox Original — green "X" inside diamond outline
    const int cx = 120, cy = 50;
    // Diamond outline (rotated square)
    for (int i = 0; i <= 28; i++) {
        display_pixel(cx + i, cy - 28 + i, true);
        display_pixel(cx - i, cy - 28 + i, true);
        display_pixel(cx + i, cy + 28 - i, true);
        display_pixel(cx - i, cy + 28 - i, true);
    }
    draw_x_mark(cx, cy, 16, false);
    draw_text_centered(120, 100, "XBOX OG", false);
}

static void splash_gc_adapter(void)
{
    // GameCube adapter — big "G" badge
    const int cx = 120, cy = 50;
    fill_circle(cx, cy, 28, false);
    fill_circle(cx, cy, 22, false);  // hollow ring? same color so just makes solid
    // Cut "G" shape via BG: pacman arc + cross stroke
    // Simple: outer circle stays, draw inner ring in BG, then add stroke
    for (int y = -22; y <= 22; y++) {
        for (int x = -22; x <= 22; x++) {
            if (x*x + y*y <= 22*22) {
                display_pixel(cx + x, cy + y, false);  // hollow center
            }
        }
    }
    // Bite from the right (pacman style)
    fill_rect(cx, cy - 4, 32, 8, false);
    // Horizontal G-stroke
    fill_rect(cx, cy - 2, 18, 4, true);
    draw_text_centered(120, 100, "GC ADAPTER", false);
}

static void splash_cdc(void)
{
    // CDC — text only ("CONFIG MODE") with a small terminal box
    draw_rect_outline(60, 30, 120, 40, false, false);
    fill_rect(62, 32, 116, 6, true);  // title bar
    draw_text_centered(120, 50, "CONFIG", false);
    draw_text_centered(120, 100, "CDC MODE", false);
}

static void splash_generic(joypad_mode_id_t mode)
{
    // Generic fallback: just a controller silhouette + mode name as
    // a number (so user can see "MODE N" and look up which one).
    const int cx = 120, cy = 50;
    // Generic dpad+buttons silhouette
    fill_rect(cx - 50, cy - 8, 100, 16, false);  // body
    fill_circle(cx - 35, cy, 10, false);          // left dpad mass
    fill_circle(cx + 35, cy, 10, false);          // right buttons mass
    // Dpad cross (cut from left circle)
    fill_rect(cx - 38, cy - 2, 6, 4, false);
    fill_rect(cx - 37, cy - 5, 4, 10, false);
    // 4 buttons (small circles on right)
    fill_circle(cx + 35, cy - 4, 2, true);
    fill_circle(cx + 39, cy,     2, true);
    fill_circle(cx + 35, cy + 4, 2, true);
    fill_circle(cx + 31, cy,     2, true);
    // Label: "MODE N"
    char buf[8] = "MODE ";
    int m = (int)mode;
    if (m >= 10) {
        buf[5] = '0' + (m / 10);
        buf[6] = '0' + (m % 10);
        buf[7] = 0;
    } else {
        buf[5] = '0' + m;
        buf[6] = 0;
    }
    draw_text_centered(120, 100, buf, false);
}

// ============================================================================
// Public entry points
// ============================================================================

void splash_render(joypad_mode_id_t mode)
{
    switch (mode) {
        case JOYPAD_MODE_SINPUT:          splash_sinput();          break;
        case JOYPAD_MODE_XINPUT:          splash_xinput();          break;
        case JOYPAD_MODE_PS3:             splash_ps3();             break;
        case JOYPAD_MODE_PS4:             splash_ps4();             break;
        case JOYPAD_MODE_SWITCH:          splash_switch();          break;
        case JOYPAD_MODE_KEYBOARD_MOUSE:  splash_keyboard_mouse();  break;
        case JOYPAD_MODE_XBONE:           splash_xbone();           break;
        case JOYPAD_MODE_XBOX_ORIGINAL:   splash_xbox_og();         break;
        case JOYPAD_MODE_GC_ADAPTER:      splash_gc_adapter();      break;
        case JOYPAD_MODE_CDC:             splash_cdc();             break;
        default:                          splash_generic(mode);     break;
    }
}

void splash_palette(joypad_mode_id_t mode, uint16_t* fg, uint16_t* pupil)
{
    // Mode-themed color pairs. FG dominates; pupil is the accent
    // (used for badge details like the "X" inside the Xbox sphere).
    switch (mode) {
        case JOYPAD_MODE_XINPUT:
        case JOYPAD_MODE_XBONE:
            *fg    = RGB5( 5, 24,  3);  // Xbox green
            *pupil = RGB5(31, 31, 31);  // white X
            break;
        case JOYPAD_MODE_XBOX_ORIGINAL:
            *fg    = RGB5( 8, 31,  8);  // brighter green diamond
            *pupil = RGB5( 5, 18,  3);  // darker green X
            break;
        case JOYPAD_MODE_PS3:
        case JOYPAD_MODE_PS4:
        case JOYPAD_MODE_PSCLASSIC:
            *fg    = RGB5(15, 18, 31);  // PlayStation blue-white
            *pupil = RGB5( 2,  6, 16);  // navy
            break;
        case JOYPAD_MODE_SWITCH:
            *fg    = RGB5(31,  4,  6);  // joycon red
            *pupil = RGB5( 3, 18, 31);  // joycon blue
            break;
        case JOYPAD_MODE_KEYBOARD_MOUSE:
            *fg    = RGB5(28, 30, 31);  // off-white
            *pupil = RGB5(10, 10, 12);  // dark grey keys
            break;
        case JOYPAD_MODE_GC_ADAPTER:
            *fg    = RGB5(18,  8, 22);  // gamecube purple
            *pupil = RGB5( 8,  3, 12);  // deeper purple
            break;
        case JOYPAD_MODE_CDC:
            *fg    = RGB5( 3, 28, 18);  // terminal green
            *pupil = RGB5( 2, 12,  8);  // deeper green
            break;
        case JOYPAD_MODE_SINPUT:
            *fg    = RGB5(28, 30, 31);  // warm snow (matches default eyes)
            *pupil = RGB5( 6,  6,  9);  // warm slate
            break;
        case JOYPAD_MODE_GBA_LINK:
            *fg    = RGB5(20, 18, 26);  // GBA-purple-ish
            *pupil = RGB5( 8,  6, 12);
            break;
        case JOYPAD_MODE_PCEMINI:
            *fg    = RGB5(31, 24, 12);  // PCE orange-cream
            *pupil = RGB5(20,  6,  2);
            break;
        case JOYPAD_MODE_HID:
        case JOYPAD_MODE_UNKNOWN:
        default:
            *fg    = RGB5(28, 30, 31);
            *pupil = RGB5( 6,  6,  9);
            break;
    }
}
