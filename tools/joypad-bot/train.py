#!/usr/bin/env python3
"""train.py — train a behavior-cloning policy on recorded gameplay.

Thin wrapper around `lerobot-train` that fills in sensible defaults for
the recorder.py dataset shape (one camera 256x240, 8-dim continuous action
representing NES button bits with sigmoid head). Pass --extra ... to
forward arbitrary lerobot-train flags through unchanged.

Usage:
    # Quick laptop test (ACT, small batch, short run)
    python3 train.py \\
        --root /Users/robert/joypad-data/tetris-nes \\
        --repo-id joypad-os/tetris-nes \\
        --output-dir /Users/robert/joypad-data/policies/tetris-act-001

    # Full GPU run
    python3 train.py --root ... --repo-id ... --output-dir ... \\
        --policy-type act --steps 100000 --batch-size 32

Policies worth trying for NES button-mask BC:
    act       — Action Chunking Transformer (small, fast, robust)
    vqbet     — VQ-VAE + transformer (good for discrete-ish actions)
    diffusion — Diffusion Policy (slower; usually better for continuous)
    smolvla   — VLM-based, multi-task ready (overkill for one game v1)
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def detect_device():
    try:
        import torch
        if torch.cuda.is_available():
            return "cuda"
        if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            return "mps"
    except ImportError:
        pass
    return "cpu"


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo-id", default="joypad-os/tetris-nes",
                   help="LeRobotDataset repo id used during recording.")
    p.add_argument("--root", required=True,
                   help="Local dataset directory written by recorder.py.")
    p.add_argument("--output-dir", required=True,
                   help="Where to save policy checkpoints + logs.")
    p.add_argument("--policy-type", default="act",
                   choices=["act", "diffusion", "vqbet", "smolvla"])
    p.add_argument("--steps", type=int, default=20000,
                   help="Training steps. 20k is a quick smoke run; "
                        "real runs want 50-200k.")
    p.add_argument("--batch-size", type=int, default=8,
                   help="Train batch size. 8 fits laptops; 32-64 on a GPU.")
    p.add_argument("--device", default=None,
                   help="cuda / mps / cpu. Defaults to auto-detect.")
    p.add_argument("--save-freq", type=int, default=5000,
                   help="Checkpoint every N steps.")
    p.add_argument("--log-freq", type=int, default=100,
                   help="Stdout log every N steps.")
    p.add_argument("--resume", action="store_true",
                   help="Resume from existing checkpoint at output-dir.")
    p.add_argument("--clean", action="store_true",
                   help="Wipe output-dir before training (forfeits any "
                        "existing checkpoints).")
    p.add_argument("--extra", nargs=argparse.REMAINDER,
                   help="Extra args forwarded verbatim to lerobot-train, "
                        "e.g. --extra --policy.dim_model=256")
    args = p.parse_args()

    if not Path(args.root).exists():
        sys.exit(f"error: dataset root not found: {args.root}")

    out = Path(args.output_dir)
    if args.clean and out.exists():
        print(f"[train] wiping {out}")
        shutil.rmtree(out)
    # lerobot-train's validate() refuses to start when the output dir
    # exists (unless --resume), so DON'T create it ourselves — only the
    # parent directory. lerobot makes the run dir itself.
    out.parent.mkdir(parents=True, exist_ok=True)

    device = args.device or detect_device()
    print(f"[train] device = {device}")

    # Use the current interpreter so we don't depend on the venv being
    # on PATH (lerobot-train script is in .venv/bin/ but parent shells
    # invoking us via .venv/bin/python don't have it on PATH).
    cmd = [
        sys.executable, "-m", "lerobot.scripts.lerobot_train",
        f"--dataset.repo_id={args.repo_id}",
        f"--dataset.root={args.root}",
        f"--policy.type={args.policy_type}",
        f"--policy.device={device}",
        f"--output_dir={out}",
        f"--steps={args.steps}",
        f"--batch_size={args.batch_size}",
        f"--save_freq={args.save_freq}",
        f"--log_freq={args.log_freq}",
        f"--policy.push_to_hub=false",
        f"--wandb.enable=false",
    ]
    if args.resume:
        cmd.append("--resume=true")
    if args.extra:
        # argparse.REMAINDER captures the leading flag too; strip if present
        extra = [a for a in args.extra if a not in ("--",)]
        cmd.extend(extra)

    print("[train] running:")
    print("  " + " \\\n    ".join(cmd))
    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
