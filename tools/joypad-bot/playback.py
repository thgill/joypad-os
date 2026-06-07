#!/usr/bin/env python3
"""playback.py — view recorded episodes from a LeRobotDataset.

Renders the captured video frame-by-frame in a pygame window with a HUD
showing the action vector recorded at that frame. Useful for sanity-
checking a dataset before training: skim through your runs, scrub to any
moment, confirm the button-press timing matches what's on screen.

Usage:
    python3 playback.py --root /Users/robert/joypad-data/tetris-nes

Controls:
    Space        play / pause
    Left/Right   step one frame
    Shift + L/R  jump 60 frames (1 s)
    Home / End   jump to start / end of episode
    PgUp / PgDn  previous / next episode
    Up / Down    change playback speed (0.25x .. 4x)
    R            re-load dataset (catch newly recorded episodes)
    G            toggle button-grid overlay
    S            save current frame as PNG to /tmp
    Esc / Q      quit
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import pygame
import torch

from lerobot.datasets.lerobot_dataset import LeRobotDataset


BUTTON_NAMES = ["A", "B", "SEL", "STA", "U", "D", "L", "R"]


def frame_to_rgb(obs_image):
    """LeRobotDataset returns observation.image as torch (C, H, W) float [0,1].
    Convert back to (H, W, C) uint8 numpy for pygame."""
    if isinstance(obs_image, torch.Tensor):
        arr = obs_image.detach().cpu().numpy()
    else:
        arr = np.asarray(obs_image)
    if arr.ndim == 3 and arr.shape[0] == 3:  # (C, H, W)
        arr = arr.transpose(1, 2, 0)
    if arr.dtype != np.uint8:
        arr = (arr * 255.0).clip(0, 255).astype(np.uint8)
    return arr


def load_episodes(ds):
    """Return list of (ep_idx, start, end, length) for each recorded episode."""
    eps = ds.meta.episodes
    out = []
    for row in eps:
        out.append({
            "idx": int(row["episode_index"]),
            "start": int(row["dataset_from_index"]),
            "end": int(row["dataset_to_index"]),
            "length": int(row["length"]),
            "task": row["tasks"][0] if row["tasks"] else "",
        })
    out.sort(key=lambda e: e["idx"])
    return out


def draw_button_hud(screen, font, action_vec, x, y, threshold=0.5):
    """Render the 8 button cells side-by-side. Bright = pressed."""
    cell_w, cell_h = 36, 22
    for i, name in enumerate(BUTTON_NAMES):
        on = float(action_vec[i]) > threshold
        bg = (50, 220, 50) if on else (40, 40, 40)
        fg = (0, 0, 0) if on else (180, 180, 180)
        rect = pygame.Rect(x + i * (cell_w + 4), y, cell_w, cell_h)
        pygame.draw.rect(screen, bg, rect, border_radius=4)
        pygame.draw.rect(screen, (90, 90, 90), rect, width=1, border_radius=4)
        text = font.render(name, True, fg)
        screen.blit(text, (rect.x + (cell_w - text.get_width()) // 2,
                            rect.y + (cell_h - text.get_height()) // 2))


def draw_hud_strip(screen, font, info, y, width):
    surf = pygame.Surface((width, 24), pygame.SRCALPHA)
    surf.fill((0, 0, 0, 180))
    screen.blit(surf, (0, y))
    screen.blit(font.render(info, True, (255, 255, 255)), (6, y + 4))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--repo-id", default="joypad-os/tetris-nes")
    p.add_argument("--root", required=True,
                   help="Local LeRobotDataset directory.")
    p.add_argument("--scale", type=int, default=3,
                   help="Display scale for the NES frame.")
    p.add_argument("--episode", type=int, default=0,
                   help="Episode index to start on.")
    p.add_argument("--start-frame", type=int, default=0,
                   help="Frame index within the episode to start on.")
    args = p.parse_args()

    if not Path(args.root).exists():
        sys.exit(f"error: dataset not found: {args.root}")

    print(f"[play] opening {args.repo_id} at {args.root}")
    ds = LeRobotDataset(args.repo_id, root=args.root)
    episodes = load_episodes(ds)
    print(f"[play] {len(episodes)} episode(s), {ds.num_frames} total frames")
    for ep in episodes:
        print(f"  ep {ep['idx']:3d}: {ep['length']:6d} frames "
              f"({ep['length']/60.0:6.1f} s)  task='{ep['task'][:50]}'")

    pygame.init()
    pygame.display.set_caption(f"joypad-bot playback — {args.repo_id}")
    font = pygame.font.SysFont("Menlo", 14)
    hud_h = 56  # top status strip + button row
    win_w = 256 * args.scale
    win_h = 240 * args.scale + hud_h
    screen = pygame.display.set_mode((win_w, win_h))

    ep_pos = max(0, min(args.episode,
                          len(episodes) - 1)) if episodes else 0
    cur = episodes[ep_pos]
    global_idx = cur["start"] + max(0, min(args.start_frame,
                                            cur["length"] - 1))
    playing = True
    speed = 1.0
    show_grid = False
    last_render_time = time.time()

    def jump(delta):
        nonlocal global_idx
        ng = global_idx + delta
        ng = max(cur["start"], min(cur["end"] - 1, ng))
        global_idx = ng

    def goto_episode(new_ep_pos):
        nonlocal ep_pos, cur, global_idx
        ep_pos = max(0, min(len(episodes) - 1, new_ep_pos))
        cur = episodes[ep_pos]
        global_idx = cur["start"]

    while True:
        # Pygame events first so input feels snappy even when paused.
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                pygame.quit(); return
            if ev.type != pygame.KEYDOWN:
                continue
            shift = ev.mod & pygame.KMOD_SHIFT
            if ev.key in (pygame.K_ESCAPE, pygame.K_q):
                pygame.quit(); return
            elif ev.key == pygame.K_SPACE:
                playing = not playing
            elif ev.key == pygame.K_LEFT:
                jump(-60 if shift else -1)
                playing = False
            elif ev.key == pygame.K_RIGHT:
                jump(60 if shift else 1)
                playing = False
            elif ev.key == pygame.K_HOME:
                global_idx = cur["start"]
            elif ev.key == pygame.K_END:
                global_idx = cur["end"] - 1
            elif ev.key == pygame.K_PAGEUP:
                goto_episode(ep_pos - 1)
            elif ev.key == pygame.K_PAGEDOWN:
                goto_episode(ep_pos + 1)
            elif ev.key == pygame.K_UP:
                speed = min(4.0, speed * 2)
            elif ev.key == pygame.K_DOWN:
                speed = max(0.125, speed / 2)
            elif ev.key == pygame.K_g:
                show_grid = not show_grid
            elif ev.key == pygame.K_r:
                print("[play] reloading dataset metadata")
                ds = LeRobotDataset(args.repo_id, root=args.root)
                episodes = load_episodes(ds)
                ep_pos = min(ep_pos, len(episodes) - 1)
                cur = episodes[ep_pos]
                global_idx = min(global_idx, cur["end"] - 1)
            elif ev.key == pygame.K_s:
                # Snapshot current frame to /tmp.
                from PIL import Image as PILImage
                sample = ds[global_idx]
                rgb = frame_to_rgb(sample["observation.image"])
                out_path = Path("/tmp") / f"playback_ep{cur['idx']}_f{global_idx - cur['start']}.png"
                PILImage.fromarray(rgb).save(out_path)
                print(f"[play] saved {out_path}")

        # Auto-advance when playing.
        now = time.time()
        if playing:
            target_dt = (1.0 / 60.0) / max(speed, 0.001)
            if now - last_render_time >= target_dt:
                jump(1)
                last_render_time = now
                if global_idx >= cur["end"] - 1:
                    playing = False

        # Fetch + render the current frame.
        sample = ds[global_idx]
        rgb = frame_to_rgb(sample["observation.image"])
        action = sample["action"].detach().cpu().numpy() \
            if isinstance(sample["action"], torch.Tensor) else sample["action"]

        screen.fill((20, 20, 20))
        surf = pygame.surfarray.make_surface(rgb.swapaxes(0, 1))
        surf = pygame.transform.scale(surf, (win_w, 240 * args.scale))
        screen.blit(surf, (0, 0))

        if show_grid:
            # Faint 8-px grid overlay (every NES tile boundary).
            grid_color = (255, 255, 255, 40)
            grid_surf = pygame.Surface((win_w, 240 * args.scale), pygame.SRCALPHA)
            for x in range(0, 256, 8):
                px = x * args.scale
                pygame.draw.line(grid_surf, grid_color, (px, 0), (px, 240 * args.scale))
            for y in range(0, 240, 8):
                py = y * args.scale
                pygame.draw.line(grid_surf, grid_color, (0, py), (win_w, py))
            screen.blit(grid_surf, (0, 0))

        # Top HUD: episode / frame / speed.
        ep_frame_idx = global_idx - cur["start"]
        info = (f"ep {cur['idx']+1}/{len(episodes)}  "
                f"frame {ep_frame_idx}/{cur['length']-1}  "
                f"t={ep_frame_idx/60.0:6.2f}s  "
                f"speed {speed:g}x  "
                f"{'[playing]' if playing else '[paused]'}")
        draw_hud_strip(screen, font, info, 240 * args.scale, win_w)

        # Bottom button row.
        draw_button_hud(screen, font, action, x=6, y=240 * args.scale + 28)

        pygame.display.flip()
        # Brief sleep so the loop doesn't spin while paused.
        if not playing:
            time.sleep(0.01)


if __name__ == "__main__":
    main()
