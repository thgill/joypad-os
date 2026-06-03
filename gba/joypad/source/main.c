// Joypad GBA multiboot payload — animated eyes + controller passthrough.
//
// The CRITICAL machinery is the joybus handshake + ResetHalt() loop, taken
// verbatim from Doridian's gba-as-controller reference
// (github.com/Doridian/Joybus-PIO). DO NOT alter it — it's what makes the
// cable's level-shifter MCU keep relaying our JOY_TRANS data instead of
// returning canned bytes (the bug that wasted hours of debugging).
//
// On top of that, we run eyes_anim in the VBlank slot. The main loop's
// ResetHalt() halts the CPU until the next VBlank IRQ fires. libgba's
// IRQ dispatcher invokes our installed handler (which advances and
// renders the eyes), then ResetHalt returns and we write
// REG_JOYTR = REG_KEYINPUT — exactly as Doridian does.

#include <gba_console.h>
#include <gba_video.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_input.h>
#include <gba_sio.h>
#include <stdint.h>

#include "display.h"
#include "eyes_anim.h"
#include "mode_marker.h"
#include "splash.h"
#include "splash_image.h"
#include "platform/platform.h"

// RGB5 packing — same as libgba's macro but local so display.c stays
// independent of libgba headers.
#define RGB5(r,g,b) (uint16_t)((r) | ((g) << 5) | ((b) << 10))

// Per-emotion palette. Background is always black; these drive the eye
// whites (FG) and the pupil dot (PUPIL). Edge midtones auto-derive in
// display.c as the average between FG/PUPIL and their neighbor colors.
//
// Color picks inspired by lilguy.net's starboy product palette
// (snow / lilac / magenta / azure / brass) — saturated FG hues paired
// with deeper-tone pupils that read clearly against the eye whites.
//
//   STATE         FG (eye white)        PUPIL
//   ─────────────────────────────────────────────────
//   IDLE/ACTIVE   warm snow             warm slate
//   SLEEP         dim grey-blue         deep navy
//   HAPPY         brass gold            molten amber
//   SAD           lilac-blue            ink indigo
//   SURPRISED     pure white            cold ink
//   ANGRY         hot magenta-red       blood crimson
//   SUSPICIOUS    sickly mint           dark forest
//   EXCITED       bright azure          deep ocean
//   CONFUSED      soft lilac            dark violet
//   WINK_L/R      warm snow             warm slate
static uint16_t color_for_state(eyes_state_t s)
{
    switch (s) {
        case EYES_STATE_HAPPY:      return RGB5(31, 25, 10);  // brass gold
        case EYES_STATE_SAD:        return RGB5(18, 22, 31);  // lilac-blue
        case EYES_STATE_SURPRISED:  return RGB5(31, 31, 31);  // pure white
        case EYES_STATE_ANGRY:      return RGB5(31,  8, 14);  // hot magenta-red
        case EYES_STATE_SUSPICIOUS: return RGB5(18, 28, 14);  // sickly mint
        case EYES_STATE_EXCITED:    return RGB5(15, 25, 31);  // bright azure
        case EYES_STATE_CONFUSED:   return RGB5(24, 18, 30);  // soft lilac
        case EYES_STATE_SLEEP:      return RGB5(12, 14, 22);  // dim grey-blue
        case EYES_STATE_BOOT:
        case EYES_STATE_IDLE:
        case EYES_STATE_ACTIVE:
        case EYES_STATE_WINK_L:
        case EYES_STATE_WINK_R:
        default:                    return RGB5(28, 30, 31);  // warm snow
    }
}

// Pupil dot colors — saturated, darker than FG, echo the emotional hue
// instead of being plain near-black. Visible against eye white but reads
// as "the pupil belongs to that mood".
static uint16_t pupil_for_state(eyes_state_t s)
{
    switch (s) {
        case EYES_STATE_HAPPY:      return RGB5(16,  9,  1);  // molten amber
        case EYES_STATE_SAD:        return RGB5( 2,  4, 14);  // ink indigo
        case EYES_STATE_SURPRISED:  return RGB5( 3,  4, 10);  // cold ink
        case EYES_STATE_ANGRY:      return RGB5(14,  0,  2);  // blood crimson
        case EYES_STATE_SUSPICIOUS: return RGB5( 2, 10,  3);  // dark forest
        case EYES_STATE_EXCITED:    return RGB5( 1,  8, 18);  // deep ocean
        case EYES_STATE_CONFUSED:   return RGB5( 9,  3, 16);  // dark violet
        case EYES_STATE_SLEEP:      return RGB5( 3,  5, 11);  // deep navy
        case EYES_STATE_BOOT:
        case EYES_STATE_IDLE:
        case EYES_STATE_ACTIVE:
        case EYES_STATE_WINK_L:
        case EYES_STATE_WINK_R:
        default:                    return RGB5( 6,  6,  9);  // warm slate
    }
}

#define REG_JOYCNTRL    (*(vu32*)0x4000140)
#define REG_JOYCTRL_RST 0b00000001

void ResetHalt() {
    if (REG_JOYCNTRL & REG_JOYCTRL_RST) {
        SystemCall(0x26);
        while (1) Halt();
        return;
    }
    Halt();
}

static volatile uint32_t vblank_count = 0;
static volatile bool     frame_ready  = false;

uint32_t platform_time_ms(void)
{
    return vblank_count * 17;
}

// Pre-handshake VBlank handler: counts frames so platform_time_ms /
// boot animation work, but does NOT touch JOYTR. The handshake below
// drives JOYTR explicitly (e.g. 0x30303030) and a stray IRQ-time write
// would clobber it, causing multi-second multiboot stalls.
static void on_vblank_pre_handshake(void)
{
    vblank_count++;
}

// Post-handshake VBlank handler: refresh JOYTR every VBlank so the host
// sees fresh KEYINPUT bits even when a long render pushes the main
// loop's own JOYTR write out by 10+ ms.
static void on_vblank(void)
{
    REG_JOYTR = REG_KEYINPUT;
    vblank_count++;
    if (frame_ready) {
        display_flip_page();
        frame_ready = false;
    }
}

// Sleep cycle — three-phase machine driven by quiet time:
//   Phase 0 ACTIVE  : input within IDLE_TO_WANDER_MS. Renders ACTIVE eyes.
//   Phase 1 WANDER  : after IDLE_TO_WANDER_MS quiet, eyes drift via IDLE
//                     state's ambient saccades.
//   Phase 2 SLEEP   : after WANDER_TO_SLEEP_MS in WANDER, eyes close
//                     (SLEEP state, half-lid breathing).
//
// SLEEP and WANDER alternate every SLEEP_HOLD_MS / WANDER_HOLD_MS until
// the user taps anything, which resets to ACTIVE.
#define IDLE_TO_WANDER_MS   5000      // 5s of quiet → start wandering
#define WANDER_TO_SLEEP_MS  180000    // 3 min wandering → fall asleep
#define SLEEP_HOLD_MS       60000     // 1 min asleep → wake to wander
#define WANDER_HOLD_MS      60000     // 1 min wandering → back to sleep

static uint32_t last_input_ms = 0;
static uint32_t phase_entered_ms = 0;
static uint8_t  idle_phase = 0;   // 0=ACTIVE, 1=WANDER (IDLE), 2=SLEEP

static void poll_input(void)
{
    uint16_t keys = (~REG_KEYINPUT) & 0x3FF;
    bool any_input = (keys != 0);

    // Treat the d-pad like a self-centering analog stick: holding a
    // direction pushes gaze that way, releasing returns to (0.5, 0.5).
    // Edge-trigger on the dpad mask so set_look() only fires when the
    // direction actually changes — otherwise we'd refresh
    // last_external_look_ms every frame and IDLE wander would never
    // kick in.
    static uint16_t prev_keys = 0;
    static uint16_t prev_dpad = 0;
    uint16_t dpad = keys & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
    if (dpad != prev_dpad) {
        float gx = 0.5f, gy = 0.5f;
        if (dpad & KEY_LEFT)  gx = 0.15f;
        if (dpad & KEY_RIGHT) gx = 0.85f;
        if (dpad & KEY_UP)    gy = 0.20f;
        if (dpad & KEY_DOWN)  gy = 0.80f;
        eyes_anim_set_look(gx, gy);
        prev_dpad = dpad;
    }
    eyes_anim_set_eyelids(
        (keys & KEY_L) ? 0.85f : 0.0f,
        (keys & KEY_R) ? 0.85f : 0.0f);

    uint16_t edges = keys & ~prev_keys;
    if (edges & KEY_A)      eyes_anim_event(EYES_EVENT_BUTTON_PRESS);
    if (edges & KEY_B)      eyes_anim_set_state(EYES_STATE_WINK_L);
    if (edges & KEY_START)  eyes_anim_set_state(EYES_STATE_HAPPY);
    if (edges & KEY_SELECT) eyes_anim_set_state(EYES_STATE_SLEEP);
    prev_keys = keys;

    uint32_t now = platform_time_ms();
    if (any_input) {
        last_input_ms = now;
        if (idle_phase != 0) {
            // Wake from WANDER or SLEEP — return to ACTIVE. The button
            // press itself drives the actual emotion change; we just
            // reset the phase tracker.
            idle_phase = 0;
            phase_entered_ms = now;
        }
    } else {
        uint32_t quiet = now - last_input_ms;
        uint32_t in_phase = now - phase_entered_ms;
        switch (idle_phase) {
            case 0:  // ACTIVE → WANDER
                if (quiet > IDLE_TO_WANDER_MS) {
                    eyes_anim_event(EYES_EVENT_IDLE_TIMEOUT);  // ACTIVE→IDLE
                    idle_phase = 1;
                    phase_entered_ms = now;
                }
                break;
            case 1:  // WANDER → SLEEP
                if (in_phase > WANDER_TO_SLEEP_MS) {
                    eyes_anim_set_state(EYES_STATE_SLEEP);
                    idle_phase = 2;
                    phase_entered_ms = now;
                }
                break;
            case 2:  // SLEEP → WANDER (cycle until input)
                if (in_phase > SLEEP_HOLD_MS) {
                    eyes_anim_set_state(EYES_STATE_IDLE);
                    idle_phase = 1;
                    phase_entered_ms = now;
                }
                break;
        }
        // If we cycled WANDER→SLEEP and back, the WANDER_HOLD_MS path
        // collapses naturally: phase 1 entered at wake-time, after
        // WANDER_HOLD_MS we re-enter SLEEP via the case 1 branch (since
        // WANDER_TO_SLEEP_MS == WANDER_HOLD_MS in current tuning).
        (void)WANDER_HOLD_MS;
    }
}

// Render a mode-specific splash badge for SPLASH_FRAMES VBlanks
// and restore Mode-4 + eye-color palette before returning. Called
// once at boot (post-handshake) and again whenever the host sends a
// splash command via JOY_RECV (USB-mode-change signal).
static void run_splash(joypad_mode_id_t splash_mode) {
    const splash_image_t* img = splash_image_for(splash_mode);
    bool used_image_mode = false;
    bool used_fallback_image = false;

    if (img) {
        splash_image_render(img);
        used_image_mode = true;
    } else {
        // No dedicated image for this mode — try the generic fallback
        // image with the mode name overlaid. This keeps every mode
        // showing a real image splash, not just the vector primitives.
        img = splash_image_fallback();
        if (img) {
            splash_image_render(img);
            splash_image_overlay_text(120, 138,
                                      splash_image_mode_name(splash_mode));
            used_image_mode = true;
            used_fallback_image = true;
            (void)used_fallback_image;
        } else {
            // No image at all — vector primitives last resort.
            uint16_t splash_fg = 0, splash_pupil = 0;
            splash_palette(splash_mode, &splash_fg, &splash_pupil);
            display_set_pupil_color(splash_pupil);
            display_set_fg_color(splash_fg);
            display_clear();
            splash_render(splash_mode);
            display_present();
        }
    }

    // Hold for SPLASH_FRAMES VBlanks, refreshing JOYTR each frame so
    // the host doesn't think we've gone silent. Also clears REG_JOYRE
    // each frame so the host's just-arrived splash command doesn't
    // immediately retrigger us if it stays in the recv register.
    uint32_t hold_until = vblank_count + SPLASH_FRAMES;
    while (vblank_count < hold_until) {
        ResetHalt();
        REG_JOYTR = REG_KEYINPUT;
    }

    if (used_image_mode) {
        // Reset DISPCNT to Mode-4 + the eyes-color palette indices
        // (0=BG, 2=FG, 3=PUPIL). The eyes loop will redo set_fg/pupil
        // for the current state on its first frame.
        display_init();
        display_clear();
        display_present();
        display_flip_page();
        display_clear();
        display_present();
    } else {
        display_flip_page();
        display_clear();
        splash_render(splash_mode);
        display_present();
    }
}

int main(void) {
    irqInit();
    irqEnable(IRQ_VBLANK);
    // Pre-handshake handler: just counts VBlanks. The post-handshake
    // handler (which writes JOYTR) gets installed AFTER the handshake
    // succeeds — installing it earlier would clobber the 0x30303030
    // handshake value the kb2040 is trying to read back.
    irqSet(IRQ_VBLANK, on_vblank_pre_handshake);

    // Display setup — replaces consoleDemoInit. We'll set DISPCNT inside
    // display_init() to MODE 4 (8-bit indexed, double-buffered) so the
    // animation can flip pages cleanly without tearing.
    display_init();
    display_clear();
    display_flip_page();
    display_clear();

    eyes_anim_init();

    // Doridian's exact handshake (Joybus-PIO upstream) — DO NOT MODIFY.
    // This is what makes the cable's MCU happy and joybus reads work
    // end-to-end.
    Halt();

    REG_JOYTR = 0x30303030;
    do { ResetHalt(); } while (REG_JSTAT & 0b00001000);

    do { ResetHalt(); } while (!(REG_JSTAT & 0b00000010));

    if (REG_JOYRE != 0x30303030) {
        Halt();
        while (1) ResetHalt();
    }

    // Handshake done — swap to the JOYTR-refreshing handler so the host
    // sees fresh KEYINPUT bits at 60 Hz even when a long render extends
    // the main loop iteration past one VBlank.
    irqSet(IRQ_VBLANK, on_vblank);

    // ────────────────────────────────────────────────────────────────
    // Boot splash — show a mode badge for ~SPLASH_FRAMES frames so the
    // user sees which USB output mode the host is currently in. Two
    // paths: image splash (Mode-3 bitmap blit) when a PNG asset exists
    // for this mode, else vector splash (Mode-4 shape primitives).
    // After SPLASH_FRAMES VBlanks we restore Mode-4 and yield to eyes.
    //
    // Refactored into run_splash() so it can also fire mid-stream when
    // the host sends a splash command via JOY_RECV (see the JOY_RECV
    // poll inside the main loop below).
    // ────────────────────────────────────────────────────────────────
    run_splash((joypad_mode_id_t)g_joypad_mode_marker.mode);

    // Fire BOOT now — after splash vanishes — so the eyes opening
    // animation plays AGAINST a freshly-cleared black screen instead
    // of running invisibly underneath the splash image. set_state in
    // eyes_anim_event resets state_start_ms to "now" so the animation
    // starts from t=0 here, not from when eyes_anim_init() ran.
    eyes_anim_event(EYES_EVENT_BOOT);

    // Input loop — Doridian's structure verbatim. ResetHalt halts CPU
    // until VBlank, our handler renders the eyes (added on top of the
    // upstream payload), then we write JOYTR. Each iteration also
    // checks JOY_RECV for a host splash command (mode-change signal).
    uint32_t last_render_vblank = 0;
    eyes_state_t last_state = EYES_STATE_COUNT;  // sentinel: forces 1st update
    // Edge-trigger on JOY_RECV changes — the cable's handshake leaves
    // a fixed sentinel in REG_JOYRE so we seed last_recv with it and
    // only act on subsequent writes from the host.
    uint32_t last_recv = REG_JOYRE;
    while (1) {
        ResetHalt();
        REG_JOYTR = REG_KEYINPUT;

        // Host splash command channel — see gba_send_splash_cmd in
        // src/native/host/gc/gba_multiboot.c. Host sends a 4-byte
        // joybus WRITE with the magic prefix 0xCAFE55XX where XX is
        // the USB output mode id. Edge-triggered so we don't loop on
        // the same value.
        uint32_t recv = REG_JOYRE;
        if (recv != last_recv) {
            last_recv = recv;
            if ((recv & 0xFFFFFF00u) == 0xCAFE5500u) {
                uint8_t cmd_mode = (uint8_t)(recv & 0xFFu);
                run_splash((joypad_mode_id_t)cmd_mode);
                // After splash, eyes state needs to be re-rendered.
                last_state = EYES_STATE_COUNT;
                last_render_vblank = vblank_count;
            }
        }

        if (vblank_count != last_render_vblank && !frame_ready) {
            last_render_vblank = vblank_count;
            poll_input();

            // Update palette when the emotional state changes. Pupil
            // color set first so the FG-edge derivation in
            // display_set_fg_color() picks up the new pupil tone.
            eyes_state_t s = eyes_anim_get_state();
            if (s != last_state) {
                display_set_pupil_color(pupil_for_state(s));
                display_set_fg_color(color_for_state(s));
                last_state = s;
            }

            if (eyes_anim_tick(platform_time_ms())) {
                eyes_anim_render();
                display_present();
            }
            frame_ready = true;
        }
    }

    return 0;
}
