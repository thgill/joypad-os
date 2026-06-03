# joypad-bot — VLM plays NES games

A baseline "AGI for gaming" loop: a vision-language model looks at the
emulator frame and decides which controller buttons to press. **No
per-game logic.** The model is told the eight NES controller buttons exist
and nothing else — it has to recognize what game it is playing and act
accordingly. Change the ROM, same code plays a different game.

## v1 architecture (this directory)

```
nes-py emulator ──▶ frame (numpy RGB)
                      │
                      ▼
        Pillow JPEG (max 384px, base64)
                      │
                      ▼
  OpenAI Chat Completions (vision + tool call)
                      │
                      ▼
        press(buttons=[...], reason="...")
                      │
                      ▼
  bit-OR'd into nes-py 8-bit action int
                      │
                      ▼
      env.step(action) × frames-per-decision
                      │
                      └─ loop
```

Self-contained — no real emulator install, no screen capture, no keyboard
injection. The fastest possible loop for proving the model can close the
"see → decide → act" cycle.

## Eventual v2 / v3 (not yet implemented)

Same `press()` tool schema, swap the effector:

- **v2** — real NES emulator (Nestopia / FCEUX / Mesen) + `mss` screen
  capture + `joypad-live` REST bridge into a Joypad OS adapter outputting
  USB HID into the same machine. Tests the full controller path.
- **v3** — real console + HDMI capture card + adapter in its console
  output mode (e.g. GameCube or Dreamcast). Same agent code; the effector
  swap is the only thing that changes.

The `press(buttons, reason)` tool schema is deliberately universal so the
agent code doesn't change across all three versions.

## Setup

```sh
pip install -r requirements.txt
export OPENAI_API_KEY=sk-...
```

## Run

```sh
python3 bot.py --rom /path/to/tetris.nes --render --save-frames out/
```

Useful flags:

| Flag                       | Default        | What it does |
|----------------------------|----------------|--------------|
| `--rom`                    | (required)     | Path to a `.nes` ROM. |
| `--model`                  | `gpt-4o-mini`  | Any OpenAI model with vision + tool calling. |
| `--frames-per-decision`    | `15`           | Emulator frames advanced per agent call (60 fps base, so 15 ≈ 250 ms game time). |
| `--max-decisions`          | `200`          | Stop after N agent calls. |
| `--render`                 | off            | Open a pyglet window to watch the agent play. |
| `--save-frames DIR`        | off            | Save every frame + `history.json` of decisions. |
| `--max-side`               | `384`          | Resize the frame to this max side before sending (latency / cost trade-off). |

## What to expect

- **Tetris L0:** kind of works. The model recognizes pieces and the board, makes reasonable but slow placements. Lines clear sometimes. Topping out within a couple of minutes is normal — the latency budget (~0.5–1 s per decision) doesn't fit per-frame play.
- **Mario 1-1:** dies fast. The model can recognize Mario and the screen but can't react fast enough to gaps.
- **Pokemon menus:** surprisingly good. Slow-paced + text-driven plays to LLM strengths.

These are baseline numbers. Real progress requires either (a) a tool
schema that does game-level moves (`place_piece(rotation, column)` for
Tetris) — but that's per-game, and we explicitly don't want that here, or
(b) a local fine-tuned model with sub-100 ms inference. (b) is the long
game.

## Cost

`gpt-4o-mini` with low-detail images at 384 px is roughly **$0.0002 per
decision**, so a 200-decision run is ~$0.04. `gpt-4o` is ~10× that.
`gpt-realtime` is more if you keep a session warm and stream voice, but
it's not the right tool for headless frame-in / JSON-out anyway.

## Why no joypad adapter in v1?

Pragmatic. The adapter sits in the path on v2/v3 where it matters
(real console output, real-world latency). In v1, every step happens on
one PC — the adapter would just be a cable in the loop. Skipping it lets
us iterate on the agent in pure software.

The *tool schema is identical*, though, which is the point: the agent
code we write now is exactly what drives the adapter later.
