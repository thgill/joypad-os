#!/usr/bin/env python3
"""manual_play.py — just play the ROM yourself in a pygame window.

No model, no recording, no menu-walker, no RAM hacks. The cleanest possible
test of "does this emulator produce the same piece sequence regardless of
how I play?" Run two games yourself, with naturally different timing, and
compare the pieces you get.

Usage:
    python3 manual_play.py --rom "/Users/robert/Downloads/Tetris (USA).nes"

Keyboard:
    Arrows  — D-pad
    Z       — A
    X       — B
    Enter   — START
    RShift  — SELECT
    R       — env.reset() (back to title)
    Esc     — quit
"""

import argparse
import sys
import time
from pathlib import Path

import pygame
from nes_py import NESEnv


BUTTON_NAMES = ["A", "B", "SELECT", "START",
                "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"]
BUTTON_BITS  = [1 << i for i in range(8)]

KEY_BINDINGS = {
    pygame.K_z:      0,
    pygame.K_x:      1,
    pygame.K_RSHIFT: 2,
    pygame.K_RETURN: 3,
    pygame.K_UP:     4,
    pygame.K_DOWN:   5,
    pygame.K_LEFT:   6,
    pygame.K_RIGHT:  7,
}

REAL_FRAME_DT = 1.0 / 60.0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True)
    p.add_argument("--scale", type=int, default=3)
    args = p.parse_args()

    if not Path(args.rom).is_file():
        sys.exit(f"error: ROM not found: {args.rom}")

    env = NESEnv(args.rom)
    pygame.init()
    pygame.display.set_caption(f"manual — {Path(args.rom).name}")
    font = pygame.font.SysFont("Menlo", 14)
    screen = pygame.display.set_mode((256 * args.scale, 240 * args.scale + 24))

    r = env.reset()
    state = r[0] if isinstance(r, tuple) else r
    frame_count = 0
    reset_count = 0

    try:
        while True:
            t_frame = time.time()

            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    return
                if ev.type == pygame.KEYDOWN:
                    if ev.key == pygame.K_ESCAPE:
                        return
                    if ev.key == pygame.K_r:
                        reset_count += 1
                        r = env.reset()
                        state = r[0] if isinstance(r, tuple) else r
                        frame_count = 0
                        print(f"[play] reset #{reset_count}")

            # Read keyboard → button mask
            keys = pygame.key.get_pressed()
            mask = 0
            for k, idx in KEY_BINDINGS.items():
                if keys[k]:
                    mask |= BUTTON_BITS[idx]

            step = env.step(mask)
            if len(step) == 5:
                state, _, term, trunc, _ = step
                done = term or trunc
            else:
                state, _, done, _ = step
            frame_count += 1
            if done:
                r = env.reset()
                state = r[0] if isinstance(r, tuple) else r

            # Render
            surf = pygame.surfarray.make_surface(state.swapaxes(0, 1))
            surf = pygame.transform.scale(
                surf, (256 * args.scale, 240 * args.scale))
            screen.blit(surf, (0, 0))
            pressed = ",".join(BUTTON_NAMES[i] for i in range(8)
                                if mask & BUTTON_BITS[i]) or "-"
            hud = f"reset#{reset_count}  frame {frame_count}  buttons={pressed}"
            hud_surf = pygame.Surface((screen.get_width(), 24), pygame.SRCALPHA)
            hud_surf.fill((0, 0, 0, 200))
            screen.blit(hud_surf, (0, 240 * args.scale))
            screen.blit(font.render(hud, True, (255, 255, 255)),
                        (6, 240 * args.scale + 5))
            pygame.display.flip()

            # 60 Hz pacing
            elapsed = time.time() - t_frame
            if elapsed < REAL_FRAME_DT:
                time.sleep(REAL_FRAME_DT - elapsed)
    finally:
        env.close()
        pygame.quit()


if __name__ == "__main__":
    main()
