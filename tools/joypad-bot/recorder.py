#!/usr/bin/env python3
"""recorder.py — record human NES gameplay to a LeRobotDataset.

You play a NES game with the keyboard in a pygame window. Every emulator
frame, the recorder writes (image, button_mask) to a LeRobotDataset on
disk. Episodes are delimited by env resets (game over) or manual R-presses.

The resulting dataset trains a behavior-cloning / VLA policy with the
lerobot toolkit (`lerobot-train ...`), and the trained policy can drop
straight into `bot.py` as a local replacement for the OpenAI call.

Keyboard:
    Arrow keys    — D-pad
    Z             — A button
    X             — B button
    Enter         — START
    Right Shift   — SELECT
    R             — end this episode + start a new one (env reset)
    P             — pause / unpause recording (keep window open)
    Esc / window close — finalize dataset + quit

Usage:
    pip install -r requirements.txt
    python3 recorder.py --rom /path/to/tetris.nes \\
        --repo-id joypad-os/tetris-nes --task "Play NES Tetris. Clear lines."
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import pygame
from nes_py import NESEnv

from lerobot.datasets.lerobot_dataset import LeRobotDataset


# Display order — also the column order in the recorded action tensor.
# This is the W3C-ish controller convention; matches NES_BUTTON_BITS in
# bot.py so the trained policy's outputs map straight to the same indices.
BUTTON_NAMES = ["A", "B", "SELECT", "START",
                "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"]
BUTTON_BITS  = [1 << i for i in range(8)]  # nes-py packs these in one uint8

# Keyboard → button-index mapping. Multiple keys per button = chord-friendly.
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

REAL_FRAME_DT = 1.0 / 60.0  # NES native frame rate


def read_buttons():
    """Read the pygame keyboard state into an 8-dim numpy float32 vector
    and the equivalent nes-py uint8 button mask."""
    keys = pygame.key.get_pressed()
    vec = np.zeros(8, dtype=np.float32)
    for k, idx in KEY_BINDINGS.items():
        if keys[k]:
            vec[idx] = 1.0
    nes_mask = 0
    for i, v in enumerate(vec):
        if v > 0.5:
            nes_mask |= BUTTON_BITS[i]
    return vec, nes_mask


def draw_hud(screen, font, info):
    """Render a translucent overlay with episode + recording state."""
    surf = pygame.Surface((screen.get_width(), 22), pygame.SRCALPHA)
    surf.fill((0, 0, 0, 160))
    screen.blit(surf, (0, 0))
    text = font.render(info, True, (255, 255, 255))
    screen.blit(text, (6, 3))


def show_frame(screen, rgb, scale, font, info):
    """Blit an NES frame and draw the HUD on top."""
    # pygame surfaces are (W, H, 3); nes-py gives (H, W, 3). swapaxes.
    surf = pygame.surfarray.make_surface(rgb.swapaxes(0, 1))
    surf = pygame.transform.scale(
        surf, (rgb.shape[1] * scale, rgb.shape[0] * scale))
    screen.blit(surf, (0, 0))
    draw_hud(screen, font, info)
    pygame.display.flip()


def build_features():
    """LeRobotDataset feature spec: one camera, one action vector.

    observation.state is a zero-filled placeholder — every lerobot policy
    we'd reasonably train on this data (ACT / Diffusion / VQBeT) hard-
    requires it even when the architecture supports state-less inputs
    (their forward passes do batch[OBS_STATE].device unconditionally).
    A (1,) zero is the smallest valid shape that keeps the API happy."""
    return {
        "observation.image": {
            "dtype": "video",
            "shape": (240, 256, 3),
            "names": ["height", "width", "channels"],
        },
        "observation.state": {
            "dtype": "float32",
            "shape": (4,),
            "names": ["dummy_0", "dummy_1", "dummy_2", "dummy_3"],
        },
        "action": {
            "dtype": "float32",
            "shape": (8,),
            "names": BUTTON_NAMES,
        },
    }


def open_dataset(repo_id, root, fps=60):
    """Create a new dataset or resume an existing one at `root`."""
    if root is not None:
        root = Path(root).expanduser()
        # If there's already a meta dir, resume so we append episodes
        # rather than blowing away prior runs.
        if (root / "meta" / "info.json").exists():
            print(f"[rec] resuming existing dataset at {root}")
            return LeRobotDataset.resume(repo_id=repo_id, root=root)
    print(f"[rec] creating new dataset {repo_id} at {root or '$HF_LEROBOT_HOME'}")
    return LeRobotDataset.create(
        repo_id=repo_id,
        fps=fps,
        features=build_features(),
        root=root,
        robot_type="nes-emulator",
        use_videos=True,
    )


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, help="Path to a .nes ROM.")
    p.add_argument("--repo-id", default="joypad-os/tetris-nes",
                   help="LeRobotDataset repo id, '{user}/{name}' form.")
    p.add_argument("--root", default=None,
                   help="Local dataset directory. Defaults to "
                        "$HF_LEROBOT_HOME/{repo-id}.")
    p.add_argument("--task", default="Play NES Tetris. Clear lines.",
                   help="Natural-language task label stored on every frame. "
                        "VLA policies key on this for multi-task routing.")
    p.add_argument("--scale", type=int, default=3,
                   help="Display scale for the pygame window.")
    args = p.parse_args()

    if not Path(args.rom).is_file():
        sys.exit(f"error: ROM not found: {args.rom}")

    # nes-py emulator + pygame window
    env = NESEnv(args.rom)
    pygame.init()
    pygame.display.set_caption(f"joypad-bot recorder — {Path(args.rom).name}")
    font = pygame.font.SysFont("Menlo", 14)
    screen = pygame.display.set_mode((256 * args.scale, 240 * args.scale))

    dataset = open_dataset(args.repo_id, args.root)

    r = env.reset()
    state = r[0] if isinstance(r, tuple) else r

    episode_idx = 0
    episode_frames = 0
    total_frames = 0
    recording = True
    quit_flag = False
    print("[rec] ready. play with arrow keys + Z/X/Enter. R = new episode, P = pause, Esc = quit.")

    try:
        while not quit_flag:
            t_frame = time.time()

            # Pump pygame events (window close, hotkeys).
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    quit_flag = True
                elif ev.type == pygame.KEYDOWN:
                    if ev.key == pygame.K_ESCAPE:
                        quit_flag = True
                    elif ev.key == pygame.K_r:
                        # End current episode and start a new one. We
                        # always save what we have so far (even one-frame
                        # episodes are fine for the dataset).
                        if episode_frames > 0:
                            print(f"[rec] saving episode {episode_idx} "
                                  f"({episode_frames} frames)")
                            dataset.save_episode()
                            episode_idx += 1
                            episode_frames = 0
                        r = env.reset()
                        state = r[0] if isinstance(r, tuple) else r
                    elif ev.key == pygame.K_p:
                        recording = not recording
                        print(f"[rec] recording = {recording}")

            if quit_flag:
                break

            # Read keyboard into action vector + nes-py mask, step env.
            action_vec, nes_mask = read_buttons()
            step = env.step(nes_mask)
            if len(step) == 5:
                state, _, term, trunc, _ = step
                done = term or trunc
            else:
                state, _, done, _ = step

            # Record (current frame, action that produced the NEXT frame).
            # By convention BC datasets store the action applied AT this
            # observation, then the resulting observation comes next frame.
            if recording:
                dataset.add_frame({
                    "observation.image": state,
                    "observation.state": np.zeros(4, dtype=np.float32),
                    "action": action_vec,
                    "task": args.task,
                })
                episode_frames += 1
                total_frames += 1

            # Render frame + HUD.
            pressed = ",".join(BUTTON_NAMES[i] for i, v in enumerate(action_vec) if v > 0.5) or "-"
            hud = (f"ep{episode_idx}  f{episode_frames}  total {total_frames}  "
                   f"rec={'ON ' if recording else 'OFF'}  buttons={pressed}")
            show_frame(screen, state, args.scale, font, hud)

            # On env-done, auto-finalize the episode and reset for the next.
            if done:
                if episode_frames > 0:
                    print(f"[rec] env done — saving episode {episode_idx} "
                          f"({episode_frames} frames)")
                    dataset.save_episode()
                    episode_idx += 1
                    episode_frames = 0
                r = env.reset()
                state = r[0] if isinstance(r, tuple) else r

            # Pace at 60 Hz so the gameplay feels right to the human player.
            elapsed = time.time() - t_frame
            if elapsed < REAL_FRAME_DT:
                time.sleep(REAL_FRAME_DT - elapsed)
    except KeyboardInterrupt:
        print("\n[rec] interrupted.")
    finally:
        # Save any in-progress episode then close out.
        if episode_frames > 0:
            print(f"[rec] saving final episode {episode_idx} "
                  f"({episode_frames} frames)")
            try:
                dataset.save_episode()
            except Exception as e:
                print(f"[rec] save_episode failed: {e}")
        try:
            dataset.finalize()
        except Exception as e:
            print(f"[rec] finalize failed: {e}")
        env.close()
        pygame.quit()
        print(f"[rec] done. total frames recorded: {total_frames} across "
              f"{episode_idx + (1 if episode_frames > 0 else 0)} episodes.")


if __name__ == "__main__":
    main()
