#!/usr/bin/env python3
"""inference.py — play an NES game with a trained lerobot policy.

Same loop shape as bot.py, but instead of calling the OpenAI API every
turn, we forward each frame through a local behavior-cloning policy and
threshold the 8-dim sigmoid output back into a NES button mask. Local
inference is fast enough (~10-50 ms) to run serially at 60 fps — no
background-emulator thread needed.

Usage:
    python3 inference.py \\
        --rom "/Users/robert/Downloads/Tetris (USA).nes" \\
        --checkpoint /Users/robert/joypad-data/policies/tetris-act-001/checkpoints/last/pretrained_model \\
        --policy-type act \\
        --render
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


# Same order as recorder.py so the trained policy's columns line up.
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


def load_policy(policy_type, checkpoint, device):
    mod_name, cls_name = POLICY_CLASSES[policy_type]
    mod = importlib.import_module(mod_name)
    cls = getattr(mod, cls_name)
    policy = cls.from_pretrained(checkpoint)
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


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, help="Path to a .nes ROM.")
    p.add_argument("--checkpoint", required=True,
                   help="Path to the trained policy directory "
                        "(e.g. .../checkpoints/last/pretrained_model).")
    p.add_argument("--policy-type", default="act", choices=list(POLICY_CLASSES))
    p.add_argument("--device", default=None,
                   help="cuda / mps / cpu. Defaults to auto-detect.")
    p.add_argument("--task", default="Play NES Tetris. Clear lines.",
                   help="Task string for VLA policies (ignored by ACT).")
    p.add_argument("--render", action="store_true",
                   help="Open a pygame window to watch the policy play.")
    p.add_argument("--scale", type=int, default=3)
    p.add_argument("--max-steps", type=int, default=0,
                   help="Stop after N steps. 0 = run until window closed.")
    p.add_argument("--threshold", type=float, default=0.5,
                   help="Sigmoid threshold for button bit-1.")
    args = p.parse_args()

    if not Path(args.rom).is_file():
        sys.exit(f"error: ROM not found: {args.rom}")
    if not Path(args.checkpoint).exists():
        sys.exit(f"error: checkpoint not found: {args.checkpoint}")

    # Device auto-detect.
    if args.device:
        device = args.device
    elif torch.cuda.is_available():
        device = "cuda"
    elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        device = "mps"
    else:
        device = "cpu"
    print(f"[inf] device={device} loading {args.policy_type} from {args.checkpoint}")
    policy = load_policy(args.policy_type, args.checkpoint, device)

    env = NESEnv(args.rom)
    if args.render:
        pygame.init()
        pygame.display.set_caption(f"joypad-bot inference — {Path(args.rom).name}")
        font = pygame.font.SysFont("Menlo", 14)
        screen = pygame.display.set_mode((256 * args.scale, 240 * args.scale))

    r = env.reset()
    state = r[0] if isinstance(r, tuple) else r

    step_count = 0
    episode_count = 0
    t_start = time.time()
    last_log = time.time()
    inf_times_ms = []

    try:
        while args.max_steps == 0 or step_count < args.max_steps:
            t_frame = time.time()

            # Pygame events / quit.
            if args.render:
                for ev in pygame.event.get():
                    if ev.type == pygame.QUIT:
                        return
                    if ev.type == pygame.KEYDOWN and ev.key == pygame.K_ESCAPE:
                        return

            # Build observation batch. Image as (1, C, H, W) float in [0,1].
            obs_image = torch.from_numpy(state).to(device).float() / 255.0
            obs_image = obs_image.permute(2, 0, 1).unsqueeze(0)
            batch = {
                "observation.image": obs_image,
                "task": [args.task],
            }

            t_inf = time.time()
            with torch.inference_mode():
                action = policy.select_action(batch)
            inf_ms = (time.time() - t_inf) * 1000
            inf_times_ms.append(inf_ms)
            if len(inf_times_ms) > 60:
                inf_times_ms.pop(0)

            action_vec = action.squeeze(0).detach().cpu().numpy()
            mask = action_to_nes_mask(action_vec, args.threshold)

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
                screen.blit(surf, (0, 0))
                pressed = ",".join(
                    BUTTON_NAMES[i] for i, v in enumerate(action_vec)
                    if float(v) > args.threshold) or "-"
                avg_inf = sum(inf_times_ms) / max(1, len(inf_times_ms))
                hud = (f"ep{episode_count}  step {step_count}  "
                       f"inf {avg_inf:.0f}ms  buttons={pressed}")
                hud_surf = pygame.Surface((screen.get_width(), 22), pygame.SRCALPHA)
                hud_surf.fill((0, 0, 0, 160))
                screen.blit(hud_surf, (0, 0))
                screen.blit(font.render(hud, True, (255, 255, 255)), (6, 3))
                pygame.display.flip()

            # Periodic stdout summary.
            if time.time() - last_log > 5.0:
                wall = time.time() - t_start
                avg_inf = sum(inf_times_ms) / max(1, len(inf_times_ms))
                print(f"[inf] step {step_count}  ep {episode_count}  "
                      f"avg_inf={avg_inf:.0f}ms  wall={wall:.0f}s")
                last_log = time.time()

            if done:
                episode_count += 1
                r = env.reset()
                state = r[0] if isinstance(r, tuple) else r
                if hasattr(policy, "reset"):
                    policy.reset()

            # Pace at 60 Hz — even with fast inference we want real-time
            # gameplay for the viewer.
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
            print(f"[inf] {step_count} steps, {episode_count} episodes, "
                  f"avg inference {avg:.1f} ms")


if __name__ == "__main__":
    main()
