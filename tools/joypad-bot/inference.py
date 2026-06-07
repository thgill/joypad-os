#!/usr/bin/env python3
"""inference.py — play an NES game with a trained lerobot policy.

The cleanest possible loop: load the ROM, observe the frame, forward
through the policy, threshold to a button mask, send to the emulator.
Repeat at 60 fps. No menu walkers, no RAM hacks, no auto-unstick — the
policy plays the unmodified ROM.

You can also drive *alongside* the bot: hold any NES-button key on the
pygame window and your input is OR'd with the policy's output for that
frame. This lets you perturb the game in real time and watch the policy
adapt — definitive proof of live inference vs. video playback.

Usage:
    python3 inference.py \\
        --rom "/Users/robert/Downloads/Tetris (USA).nes" \\
        --checkpoint /Users/robert/joypad-data/policies/tetris-act-001/checkpoints/last/pretrained_model \\
        --render

Keyboard (with --render):
    Arrows   D-pad        Z   A button       X   B button
    Enter    START        RShift  SELECT     Esc quit
    (held keys OR with policy output — your inputs add to the policy's)
"""

import argparse
import importlib
import sys
import time
from pathlib import Path

import numpy as np
import pygame
import torch
from nes_py import NESEnv


BUTTON_NAMES = ["A", "B", "SELECT", "START",
                "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"]
BUTTON_BITS  = [1 << i for i in range(8)]
REAL_FRAME_DT = 1.0 / 60.0

POLICY_CLASSES = {
    "act":       ("lerobot.policies.act.modeling_act",       "ACTPolicy"),
    "diffusion": ("lerobot.policies.diffusion.modeling_diffusion", "DiffusionPolicy"),
    "vqbet":     ("lerobot.policies.vqbet.modeling_vqbet",   "VQBeTPolicy"),
    "smolvla":   ("lerobot.policies.smolvla.modeling_smolvla", "SmolVLAPolicy"),
}

KEY_BINDINGS = {
    pygame.K_z:      0,   # A
    pygame.K_x:      1,   # B
    pygame.K_RSHIFT: 2,   # SELECT
    pygame.K_RETURN: 3,   # START
    pygame.K_UP:     4,
    pygame.K_DOWN:   5,
    pygame.K_LEFT:   6,
    pygame.K_RIGHT:  7,
}

HUD_BUTTON_LABELS = ["A", "B", "SEL", "STA", "U", "D", "L", "R"]


def load_policy(policy_type, checkpoint, device,
                  num_inference_steps=None, n_action_steps=None):
    mod_name, cls_name = POLICY_CLASSES[policy_type]
    mod = importlib.import_module(mod_name)
    cls = getattr(mod, cls_name)
    policy = cls.from_pretrained(checkpoint)
    # Speed knobs for Diffusion Policy — DDPM defaults to 100 denoising
    # steps per inference which is way too slow for real-time play. 10
    # steps is roughly as good and ~10x faster.
    if num_inference_steps is not None and hasattr(policy.config, "num_inference_steps"):
        policy.config.num_inference_steps = num_inference_steps
    if n_action_steps is not None and hasattr(policy.config, "n_action_steps"):
        policy.config.n_action_steps = n_action_steps
    policy.to(device)
    policy.eval()
    if hasattr(policy, "reset"):
        policy.reset()
    return policy


def action_to_nes_mask(action_vec, threshold=0.5):
    mask = 0
    for i, v in enumerate(action_vec):
        if float(v) > threshold:
            mask |= BUTTON_BITS[i]
    return mask


def draw_status_strip(screen, font, info, y, width):
    surf = pygame.Surface((width, 24), pygame.SRCALPHA)
    surf.fill((0, 0, 0, 200))
    screen.blit(surf, (0, y))
    screen.blit(font.render(info, True, (255, 255, 255)), (6, y + 4))


def draw_button_grid(screen, font, font_small, action_vec, threshold,
                      manual_mask, x, y):
    """8 button cells. Green = policy pressing; blue outline = you pressing.
    Numeric under each cell is the policy's raw sigmoid confidence."""
    cell_w, cell_h = 50, 26
    for i, label in enumerate(HUD_BUTTON_LABELS):
        v = float(action_vec[i])
        policy_on = v > threshold
        manual_on = bool(manual_mask & BUTTON_BITS[i])
        if policy_on:
            bg = (40, 220, 70); fg = (0, 0, 0)
        else:
            shade = int(round(40 + max(0.0, min(1.0, v)) * 80))
            bg = (shade, shade, shade); fg = (210, 210, 210)
        rect = pygame.Rect(x + i * (cell_w + 4), y, cell_w, cell_h)
        pygame.draw.rect(screen, bg, rect, border_radius=5)
        # blue outline when the user is holding this key
        border_color = (80, 160, 255) if manual_on else (100, 100, 100)
        border_w = 3 if manual_on else 1
        pygame.draw.rect(screen, border_color, rect, width=border_w, border_radius=5)
        text = font.render(label, True, fg)
        screen.blit(text, (rect.x + (cell_w - text.get_width()) // 2, rect.y + 2))
        val = font_small.render(f"{v:.2f}", True, fg)
        screen.blit(val, (rect.x + (cell_w - val.get_width()) // 2, rect.y + 14))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, help="Path to a .nes ROM.")
    p.add_argument("--checkpoint", required=True,
                   help="Path to the trained policy directory.")
    p.add_argument("--policy-type", default="act", choices=list(POLICY_CLASSES))
    p.add_argument("--device", default=None,
                   help="cuda / mps / cpu. Defaults to auto-detect.")
    p.add_argument("--task", default="Play NES Tetris. Clear lines.")
    p.add_argument("--render", action="store_true")
    p.add_argument("--scale", type=int, default=3)
    p.add_argument("--max-steps", type=int, default=0,
                   help="Stop after N steps. 0 = run until window closed.")
    p.add_argument("--threshold", type=float, default=0.5,
                   help="Sigmoid threshold for button-press.")
    p.add_argument("--noise", type=float, default=0.0,
                   help="Std-dev of Gaussian noise added to action vector "
                        "before thresholding. 0 = clean deterministic.")
    p.add_argument("--diffusion-steps", type=int, default=10,
                   help="Number of denoising steps for Diffusion Policy. "
                        "Default 10 (fast, ~10x speedup over the 100 default "
                        "with similar quality). Set higher for more accuracy.")
    p.add_argument("--action-steps", type=int, default=None,
                   help="Override n_action_steps — how many actions are "
                        "popped from a chunk before re-querying. Larger = "
                        "less frequent inference = higher effective fps.")
    args = p.parse_args()

    if not Path(args.rom).is_file():
        sys.exit(f"error: ROM not found: {args.rom}")
    if not Path(args.checkpoint).exists():
        sys.exit(f"error: checkpoint not found: {args.checkpoint}")

    # Device auto-detect
    if args.device:
        device = args.device
    elif torch.cuda.is_available():
        device = "cuda"
    elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        device = "mps"
    else:
        device = "cpu"
    print(f"[inf] device={device} loading {args.policy_type} from {args.checkpoint}")
    policy = load_policy(args.policy_type, args.checkpoint, device,
                          num_inference_steps=args.diffusion_steps,
                          n_action_steps=args.action_steps)

    env = NESEnv(args.rom)
    HUD_H = 60
    if args.render:
        pygame.init()
        pygame.display.set_caption(f"joypad-bot — {Path(args.rom).name}")
        font = pygame.font.SysFont("Menlo", 14, bold=True)
        font_small = pygame.font.SysFont("Menlo", 11)
        screen = pygame.display.set_mode(
            (256 * args.scale, 240 * args.scale + HUD_H))

    r = env.reset()
    state = r[0] if isinstance(r, tuple) else r

    step_count = 0
    policy_press_frames = 0
    manual_press_frames = 0
    final_press_frames = 0
    final_press_button_counts = {n: 0 for n in BUTTON_NAMES}
    episode_count = 0
    t_start = time.time()
    last_log = time.time()
    inf_times_ms = []
    # Track which NES buttons the user is currently holding via explicit
    # KEYDOWN/KEYUP events — more reliable on macOS than pygame.key.get_pressed.
    manual_held = set()

    try:
        while args.max_steps == 0 or step_count < args.max_steps:
            t_frame = time.time()

            # Window events + keyboard state tracking.
            if args.render:
                for ev in pygame.event.get():
                    if ev.type == pygame.QUIT:
                        return
                    if ev.type == pygame.KEYDOWN:
                        if ev.key == pygame.K_ESCAPE:
                            return
                        if ev.key in KEY_BINDINGS:
                            manual_held.add(KEY_BINDINGS[ev.key])
                    if ev.type == pygame.KEYUP:
                        if ev.key in KEY_BINDINGS:
                            manual_held.discard(KEY_BINDINGS[ev.key])

            # Build observation: image (1,C,H,W) + dummy state.
            obs_image = torch.from_numpy(state.copy()).to(device).float() / 255.0
            obs_image = obs_image.permute(2, 0, 1).unsqueeze(0)
            obs_state = torch.zeros((1, 4), device=device, dtype=torch.float32)
            batch = {
                "observation.image": obs_image,
                "observation.state": obs_state,
                "task": [args.task],
            }

            # Forward pass.
            t_inf = time.time()
            with torch.inference_mode():
                action = policy.select_action(batch)
            inf_ms = (time.time() - t_inf) * 1000
            inf_times_ms.append(inf_ms)
            if len(inf_times_ms) > 60:
                inf_times_ms.pop(0)

            action_vec = action.squeeze(0).detach().cpu().numpy()
            if args.noise > 0:
                action_vec = action_vec + np.random.normal(
                    0, args.noise, size=action_vec.shape).astype(np.float32)
            policy_mask = action_to_nes_mask(action_vec, args.threshold)
            if policy_mask != 0:
                policy_press_frames += 1

            # Mix manual + policy. User's input WINS on the same axis but
            # the model can still drive non-conflicting buttons:
            #   - User holds LEFT  → model's LEFT and RIGHT discarded
            #   - User holds RIGHT → same, your direction wins
            #   - User holds UP    → model's UP/DOWN discarded
            #   - User holds DOWN  → same, your axis wins
            #   - Other buttons (A, B, START, SELECT) OR with the policy
            # Release all keys → policy resumes fully.
            manual_mask = 0
            for idx in manual_held:
                manual_mask |= BUTTON_BITS[idx]
            LEFT  = BUTTON_BITS[BUTTON_NAMES.index("DPAD_LEFT")]
            RIGHT = BUTTON_BITS[BUTTON_NAMES.index("DPAD_RIGHT")]
            UP    = BUTTON_BITS[BUTTON_NAMES.index("DPAD_UP")]
            DOWN  = BUTTON_BITS[BUTTON_NAMES.index("DPAD_DOWN")]
            mask = policy_mask
            if manual_mask & (LEFT | RIGHT):
                mask &= ~(LEFT | RIGHT)
            if manual_mask & (UP | DOWN):
                mask &= ~(UP | DOWN)
            mask |= manual_mask

            # COMPREHENSIVE tracking: count ANYTHING that reaches env.step.
            if manual_mask != 0:
                manual_press_frames += 1
            if mask != 0:
                final_press_frames += 1
                for i in range(8):
                    if mask & BUTTON_BITS[i]:
                        final_press_button_counts[BUTTON_NAMES[i]] += 1
                # Print every single frame where ANY button reaches the game.
                pressed = [BUTTON_NAMES[i] for i in range(8) if mask & BUTTON_BITS[i]]
                pm = [BUTTON_NAMES[i] for i in range(8) if policy_mask & BUTTON_BITS[i]]
                mm = [BUTTON_NAMES[i] for i in range(8) if manual_mask & BUTTON_BITS[i]]
                print(f"[inf] step {step_count}: SENT {pressed}  "
                      f"(policy={pm or '-'}, manual={mm or '-'})")

            # Step emulator.
            step = env.step(mask)
            if len(step) == 5:
                state, _, term, trunc, _ = step
                done = term or trunc
            else:
                state, _, done, _ = step
            step_count += 1

            # Render.
            if args.render:
                surf = pygame.surfarray.make_surface(state.swapaxes(0, 1))
                surf = pygame.transform.scale(
                    surf, (256 * args.scale, 240 * args.scale))
                screen.fill((0, 0, 0))
                screen.blit(surf, (0, 0))

                avg_inf = sum(inf_times_ms) / max(1, len(inf_times_ms))
                wall = int(time.time() - t_start)
                fps = step_count / max(1.0, time.time() - t_start)
                manual_tag = "  +MANUAL" if manual_mask else ""
                info = (f"{args.policy_type.upper()}  "
                        f"step {step_count}  ep {episode_count}  "
                        f"inf {avg_inf:.0f}ms  {fps:.0f} fps  "
                        f"wall {wall//60:d}:{wall%60:02d}{manual_tag}")
                draw_status_strip(screen, font, info,
                                   y=240 * args.scale,
                                   width=screen.get_width())

                grid_w = 8 * 50 + 7 * 4
                grid_x = (screen.get_width() - grid_w) // 2
                draw_button_grid(screen, font, font_small,
                                  action_vec, args.threshold, manual_mask,
                                  x=grid_x, y=240 * args.scale + 28)
                pygame.display.flip()

            if time.time() - last_log > 5.0:
                avg_inf = sum(inf_times_ms) / max(1, len(inf_times_ms))
                wall = time.time() - t_start
                print(f"[inf] step {step_count}  ep {episode_count}  "
                      f"avg_inf={avg_inf:.0f}ms  wall={wall:.0f}s")
                last_log = time.time()

            if done:
                episode_count += 1
                r = env.reset()
                state = r[0] if isinstance(r, tuple) else r
                if hasattr(policy, "reset"):
                    policy.reset()

            # 60 Hz pacing.
            elapsed = time.time() - t_frame
            if elapsed < REAL_FRAME_DT:
                time.sleep(REAL_FRAME_DT - elapsed)
    except KeyboardInterrupt:
        print("\n[inf] interrupted.")
    finally:
        env.close()
        if args.render:
            pygame.quit()
        if inf_times_ms:
            avg = sum(inf_times_ms) / len(inf_times_ms)
            print(f"\n[inf] === final tally ===")
            print(f"[inf] {step_count} env.step calls, {episode_count} episodes, "
                  f"avg inference {avg:.1f} ms")
            print(f"[inf] policy emitted any button:  {policy_press_frames} frames "
                  f"({100*policy_press_frames/max(1,step_count):.1f}%)")
            print(f"[inf] keyboard emitted any button: {manual_press_frames} frames "
                  f"({100*manual_press_frames/max(1,step_count):.1f}%)")
            print(f"[inf] FINAL mask sent to game:    {final_press_frames} frames "
                  f"({100*final_press_frames/max(1,step_count):.1f}%)")
            print(f"[inf] per-button press counts in final mask:")
            for n, c in final_press_button_counts.items():
                print(f"  {n:12s}: {c:5d} ({100*c/max(1,step_count):.1f}%)")


if __name__ == "__main__":
    main()
