// pico/rand.h shim for CH32V307
//
// libxsm3 (Xbox 360 security) calls get_rand_32() for its challenge nonce. The
// CH32V307 has no dedicated TRNG peripheral exposed here, so seed a small LCG
// from the on-chip unique ID + free-running SysTick. This is NOT cryptographic,
// but XSM3's auth handshake only needs non-repeating values, and the real
// security is in the (static) key material, not this nonce.

#ifndef PICO_RAND_H_SHIM
#define PICO_RAND_H_SHIM

#include <stdint.h>

static inline uint32_t get_rand_32(void)
{
    extern uint32_t platform_time_us(void);
    static uint32_t state = 0;
    if (state == 0) {
        const volatile uint32_t* uid = (const volatile uint32_t*) 0x1FFFF7E8UL;
        state = uid[0] ^ uid[1] ^ uid[2] ^ 0x9E3779B9u;
    }
    // xorshift32 mixed with the live microsecond counter
    state ^= platform_time_us();
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

#endif
