// ws2812_wch.c - LED backend for CH32V307 usb2usb
//
// The CH32V307-EVT board has no addressable RGB LED, just a plain GPIO LED on
// PC0 (board KEY/LED header). leds.c drives status through the NeoPixel API, so
// this backend maps that single-LED status onto PC0, mirroring the plain-GPIO
// LED behaviour in leds.c (board_led_task) used on Pico boards:
//   - Solid on:           one or more controllers connected
//   - Slow blink (500ms):  idle (no controller) — also a "firmware alive" beat
//   - Profile indicator:   N fast blinks then pause (N = profile_index + 1)
//
// The board LED on PC0 is active-low (anode to VCC, cathode to PC0): lit when
// PC0 is driven LOW. The BSP configures PC0 as open-drain, which would work for
// active-low, but neopixel_init() reconfigures it push-pull so the polarity is
// explicit and LED_ACTIVE_HIGH can be flipped for a cathode-to-GND board.

#include "core/services/leds/neopixel/ws2812.h"
#include "platform/platform.h"
#include "ch32v30x.h"

#define LED_GPIO_PORT   GPIOC
#define LED_GPIO_PIN    GPIO_Pin_0
#define LED_ACTIVE_HIGH 0  // board LED on PC0 is active-low (lit when PC0 driven LOW)

static bool     led_inited = false;
static bool     led_state = false;
static uint32_t led_last_toggle = 0;
static uint8_t  led_blink_count = 0;
static uint32_t led_indicate_start = 0;

static void led_set(bool on)
{
    if (!led_inited) return;
#if LED_ACTIVE_HIGH
    if (on) GPIO_SetBits(LED_GPIO_PORT, LED_GPIO_PIN);
    else    GPIO_ResetBits(LED_GPIO_PORT, LED_GPIO_PIN);
#else
    if (on) GPIO_ResetBits(LED_GPIO_PORT, LED_GPIO_PIN);
    else    GPIO_SetBits(LED_GPIO_PORT, LED_GPIO_PIN);
#endif
    led_state = on;
}

void neopixel_init(void)
{
    // GPIOC clock is already enabled by the BSP board_init (LED_CLOCK_EN), but
    // enable it again so init order doesn't matter. Reconfigure PC0 push-pull.
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = LED_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &gpio);
    led_inited = true;
    led_set(false);
}

void neopixel_task(int pat)
{
    if (!led_inited) return;
    uint32_t now = platform_time_ms();

    // Profile indicator: N fast blinks (150ms on/off) then a 600ms pause.
    if (led_blink_count > 0) {
        uint32_t elapsed = now - led_indicate_start;
        uint32_t blink_duration = led_blink_count * 300;
        if (elapsed < blink_duration) {
            uint32_t phase = (elapsed / 150) % 2;
            uint32_t blink_num = elapsed / 300;
            led_set(blink_num < led_blink_count && phase == 0);
        } else if (elapsed < blink_duration + 600) {
            led_set(false);  // pause
        } else {
            led_blink_count = 0;  // done
        }
        return;
    }

    if (pat > 0) {
        led_set(true);                 // connected — solid on
    } else if (now - led_last_toggle >= 500) {
        led_last_toggle = now;         // idle — slow heartbeat blink
        led_set(!led_state);
    }
}

void neopixel_indicate_profile(uint8_t profile_index)
{
    led_blink_count = profile_index + 1;
    led_indicate_start = platform_time_ms();
}

bool neopixel_is_indicating(void) { return led_blink_count > 0; }

void neopixel_disable(void) { led_set(false); led_inited = false; }

// Unused on a single plain LED — RGB/color APIs are no-ops here.
void neopixel_set_pin(int8_t pin) { (void)pin; }
void neopixel_set_custom_colors(const uint8_t colors[][3], uint8_t count) { (void)colors; (void)count; }
bool neopixel_has_custom_colors(void) { return false; }
void neopixel_set_pulse_mask(uint16_t mask) { (void)mask; }
void neopixel_set_press_mask(uint16_t mask) { (void)mask; }
void neopixel_set_override_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
