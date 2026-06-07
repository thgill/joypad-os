#!/usr/bin/env python3
"""patch_add_state.py — add a dummy observation.state column to a dataset.

The lerobot policies (ACT, Diffusion, VQBeT) hard-require observation.state
even when the architecture supports image-only inputs. If you recorded a
dataset before recorder.py was fixed to include observation.state, run
this once to retro-fit the column + metadata in place.

Usage:
    python3 patch_add_state.py --root /Users/robert/joypad-data/tetris-nes
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd


def patch_info(root):
    """Add or overwrite observation.state in features."""
    info_path = root / "meta" / "info.json"
    info = json.loads(info_path.read_text())
    info["features"]["observation.state"] = {
        "dtype": "float32",
        "shape": [4],
        "names": ["dummy_0", "dummy_1", "dummy_2", "dummy_3"],
    }
    info_path.write_text(json.dumps(info, indent=4))
    print(f"  info.json: added observation.state feature")
    return True


def patch_stats(root):
    """Add or overwrite observation.state stats. All zeros is fine — the
    column itself is all zeros, so min/max/mean=0 and std=1 (avoid /0 in
    normalization) match exactly."""
    stats_path = root / "meta" / "stats.json"
    stats = json.loads(stats_path.read_text())
    stats["observation.state"] = {
        "min":   [0.0, 0.0, 0.0, 0.0],
        "max":   [0.0, 0.0, 0.0, 0.0],
        "mean":  [0.0, 0.0, 0.0, 0.0],
        "std":   [1.0, 1.0, 1.0, 1.0],
        "count": [stats.get("action", {}).get("count", [0])[0]],
    }
    stats_path.write_text(json.dumps(stats, indent=4))
    print(f"  stats.json: added observation.state stats")
    return True


def patch_parquet(root):
    """Add observation.state column to each data parquet (zero (1,) vectors)."""
    data_dir = root / "data"
    parquet_files = sorted(data_dir.rglob("*.parquet"))
    if not parquet_files:
        print(f"  no parquet files found under {data_dir}")
        return False
    changed = 0
    for pq in parquet_files:
        df = pd.read_parquet(pq)
        if "observation.state" in df.columns:
            # Already added — but the prior buggy run wrote shape (1,);
            # rewrite as (4,) so the HF datasets cast works.
            sample = df["observation.state"].iloc[0]
            if hasattr(sample, "__len__") and len(sample) == 4:
                print(f"  {pq.relative_to(root)}: already has observation.state(4,), skipping")
                continue
            else:
                print(f"  {pq.relative_to(root)}: REPLACING bad observation.state column")
        zeros = [np.zeros(4, dtype=np.float32) for _ in range(len(df))]
        df["observation.state"] = zeros
        df.to_parquet(pq, index=False)
        print(f"  {pq.relative_to(root)}: wrote observation.state(4,) ({len(df)} rows)")
        changed += 1
    return changed > 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True,
                   help="Local LeRobotDataset directory.")
    args = p.parse_args()

    root = Path(args.root).expanduser()
    if not (root / "meta" / "info.json").exists():
        sys.exit(f"error: not a lerobot dataset (no meta/info.json): {root}")

    print(f"[patch] {root}")
    patch_info(root)
    patch_stats(root)
    patch_parquet(root)
    print("[patch] done. you can now train against this dataset.")


if __name__ == "__main__":
    main()
