#!/usr/bin/env python3
"""joypad-bot — VLM plays NES games through a controller-only tool schema.

v1 architecture (this file): nes-py emulator + OpenAI vision API, pure
software loop. The model sees a frame and emits a single press() tool call
holding a list of controller buttons; the effector translates that into the
emulator's next step. No game-specific logic, no per-game prompt — change
the ROM and the same code plays a different game.

Eventual v2: swap the NESPyEffector for a JoypadLiveEffector (HTTP POSTs
into the joypad-live REST bridge) — same agent code, same tool schema, real
adapter driving any console. Eventual v3: same loop, real console + HDMI
capture replacing the emulator frame source. The press() abstraction is
deliberately the universal controller-side interface for all three paths.

Usage:
    pip install -r requirements.txt
    export OPENAI_API_KEY=sk-...
    python3 bot.py --rom /Users/robert/Downloads/tetris.nes --render
"""

import argparse
import base64
import io
import json
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from nes_py import NESEnv
from openai import OpenAI
from PIL import Image

# 60 Hz wall-clock pacing during action execution so the emulator runs at
# real time (otherwise nes-py blasts through frames as fast as it can).
REAL_FRAME_DT = 1.0 / 60.0

# pygame is optional — only imported if --render is passed. nes-py's own
# render_mode='human' silently no-ops on macOS, so we run our own viewer.
_pygame = None
_pg_screen = None


# --- Controller -------------------------------------------------------------
# Game-agnostic NES controller schema. The agent never sees Tetris-specific
# concepts; it only knows these eight buttons exist.

NES_BUTTON_BITS = {
    "A":          1 << 0,
    "B":          1 << 1,
    "SELECT":     1 << 2,
    "START":      1 << 3,
    "DPAD_UP":    1 << 4,
    "DPAD_DOWN":  1 << 5,
    "DPAD_LEFT":  1 << 6,
    "DPAD_RIGHT": 1 << 7,
}


def buttons_to_action(buttons):
    """Translate a list of controller buttons into nes-py's 8-bit action int."""
    a = 0
    for b in buttons:
        a |= NES_BUTTON_BITS.get(b, 0)
    return a


# --- Tool schema ------------------------------------------------------------
# Single tool, controller-only. The model is told nothing about what game it
# is playing — it has to recognize the screen and figure out what to press.

BASE_SYSTEM_PROMPT = """You are playing a video game on a real NES through a standard \
controller: DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, A, B, START, SELECT.

HOW THE LOOP WORKS
==================
The emulator runs at 60 fps continuously. When your play() call arrives,
the runtime presses each action's buttons for `hold_frames` frames (so
hold_frames=60 == 1 second of holding that button). Between your decisions,
the emulator KEEPS RUNNING — falling pieces keep falling, enemies keep
moving — so an unfinished plan does NOT freeze and wait for you.

LEARN-BY-DOING (first few turns)
================================
Different games have non-obvious input semantics — Delayed Auto-Shift on
the d-pad, soft-drop vs hard-drop on DOWN, A vs B rotation direction,
gravity per level, etc. You don't know these for THIS game yet.

For the first ~5 gameplay turns, treat play as EXPERIMENTATION:
- Use SHORT exploratory actions (e.g. one (DPAD_LEFT, 6f) press) and watch
  what actually happens to the piece on the next turn.
- Record what you learn in the 'knowledge' field of the play() tool.
  Examples: 'A rotates CW; 6f LEFT shifts piece exactly 1 cell; DOWN is
  soft-drop (~1 row per 4f at level 0); DAS auto-repeats after 16f hold'.
- The runtime feeds 'knowledge' back to you every subsequent turn — so it
  PERSISTS. Re-emit and EXTEND it each turn rather than discarding it.
- After ~5 turns of observation you should have enough calibration data to
  start committing precise placements (next section).

If your knowledge field contains substantive observations already, you can
skip experimentation and use what you know.

COMMIT TO A FULL MOVE — DO NOT SECOND-GUESS
============================================
Once calibrated (i.e. your 'knowledge' field describes input timing),
each turn places ONE piece completely. From ONE screenshot you can see
the current piece, its position, the NEXT piece in the preview, and the
current stack — ALL the information needed to decide the entire placement.

- Pick the FINAL column and rotation for the current piece.
- Emit the COMPLETE sequence to commit it:
  1. Rotations (A or B, 4-6 hold_frames each).
  2. Horizontal moves (DPAD_LEFT / DPAD_RIGHT, ~6 hold_frames per cell).
  3. **HARD DROP: DPAD_DOWN for 60+ hold_frames** so the piece slams all
     the way to the bottom of the stack and locks. Do NOT use a 6-frame
     DOWN — that just nudges the piece one row.
  4. A short wait (15-30 hold_frames, empty buttons) so the next piece
     spawns before your next decision fires.

ONE decision == ONE PIECE PLACED. Do NOT emit tentative half-moves and
re-plan next decision — that creates random chaos because the piece keeps
falling while you re-think. If your last placement was wrong, accept it
and plan the NEXT piece properly using the now-visible NEXT preview.

CRITICAL RULES
==============
- **NEVER press START during gameplay.** START PAUSES on almost every NES
  game. Once you see a falling piece / active sprites, START is off-limits.
- **If you see PAUSE text on the gameplay screen, press START exactly ONCE
  to unpause.** Games do NOT auto-unpause. State this in the 'screen'
  field (e.g. "Paused gameplay screen with PAUSE overlay") so the runtime
  knows to let START through.
- **Use START/SELECT only on title/menu screens** (or to unpause).

PLAY LOOP
=========
Each turn:
1. Describe the screen: kind (title/menu/gameplay/paused/game-over), current
   piece + rotation, NEXT piece, stack shape, anything highlighted.
2. State your goal: where this piece is going (final column + rotation).
3. Emit the FULL action sequence to commit that placement, ending with a
   long DPAD_DOWN hard-drop + short wait. Use the play() tool."""


GAME_BRIEF_PROMPT = """You are about to play an NES game. Look at this screen — \
it is the very first frame the emulator shows (typically a title screen, \
copyright screen, or boot logo).

Identify the game (be specific — title, version if visible). Then write a
concise STRATEGY BRIEF that an agent could follow to actually play it well:

- Game name and genre.
- Objective (how the player wins or progresses).
- Core mechanics (what each button does — A/B/START/SELECT/d-pad).
- Key strategy points (heuristics a competent player uses moment-to-moment).
- Common pitfalls (what loses or stalls the game).
- For menus/setup screens before the actual game starts, brief instructions on
  what selections lead to the best baseline gameplay session.

Keep it tight — under 300 words. The brief gets injected into every subsequent
decision, so it should be reference material, not narrative. If you cannot
identify the game from this frame, say so and give general guidance for the
genre you think it is."""


TOOLS = [{
    "type": "function",
    "function": {
        "name": "play",
        "description": (
            "Execute a sequence of controller actions in response to what's on "
            "screen. Each action holds the listed buttons for hold_frames "
            "emulator frames (60 fps), then releases. Use multiple actions to "
            "plan ahead from a single screenshot — e.g. for a falling piece "
            "you can emit the entire 'rotate, shift, drop' sequence in one "
            "call instead of one button per call. Empty buttons = wait."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "screen": {
                    "type": "string",
                    "description": (
                        "One-sentence concrete description of what's on the "
                        "current screen — kind of screen (title, menu, "
                        "gameplay, paused, etc.) and what's notable / "
                        "highlighted / happening."
                    ),
                },
                "goal": {
                    "type": "string",
                    "description": (
                        "One sentence on what should happen next for the "
                        "game to progress."
                    ),
                },
                "knowledge": {
                    "type": "string",
                    "description": (
                        "Your accumulated knowledge about this specific game's "
                        "input semantics, learned by experimentation. The "
                        "runtime feeds this field back to you on every "
                        "subsequent turn so it persists across decisions. "
                        "REWRITE the entire field each turn, incorporating "
                        "new observations. Examples of what to record: 'A "
                        "rotates clockwise; B counter-clockwise', 'pressing "
                        "LEFT for 6 frames moves piece exactly 1 cell', "
                        "'DOWN is soft-drop (~3 rows per 30f at level 0), "
                        "not hard-drop', 'DAS kicks in after 16 frames of "
                        "holding then auto-repeats every 6 frames'. Keep "
                        "under ~500 chars; this is reference material."
                    ),
                },
                "actions": {
                    "type": "array",
                    "description": (
                        "Ordered sequence of (buttons, hold_frames). "
                        "Executed sequentially. Up to ~12 actions per call."
                    ),
                    "items": {
                        "type": "object",
                        "properties": {
                            "buttons": {
                                "type": "array",
                                "items": {
                                    "type": "string",
                                    "enum": list(NES_BUTTON_BITS.keys()),
                                },
                            },
                            "hold_frames": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 120,
                                "default": 6,
                                "description": (
                                    "How many emulator frames to HOLD this "
                                    "button (60 fps base). For a hard-drop, "
                                    "use 60-90 on DPAD_DOWN. For a single "
                                    "rotation or column shift, 4-6 is enough."
                                ),
                            },
                        },
                        "required": ["buttons"],
                    },
                    "maxItems": 12,
                },
            },
            "required": ["screen", "goal", "actions"],
        },
    },
}]


# --- Frame encoding ---------------------------------------------------------

# --- Viewer (pygame, optional) ----------------------------------------------

def viewer_init(scale=3, title="joypad-bot"):
    """Open a pygame window for live viewing. nes-py's own render_mode='human'
    silently fails to open a window from many macOS contexts (subprocess
    spawned by AppleScript, etc.); pygame/SDL2 is rock solid in the same
    contexts, so we run our own."""
    global _pygame, _pg_screen
    import pygame
    _pygame = pygame
    pygame.init()
    _pg_screen = pygame.display.set_mode((256 * scale, 240 * scale))
    pygame.display.set_caption(title)
    return _pg_screen


def viewer_show(rgb_array, scale=3):
    """Blit a (240, 256, 3) RGB numpy frame onto the pygame window."""
    if _pygame is None:
        return False
    # pygame surfaces are (W, H, 3); numpy frame is (H, W, 3). swapaxes(0,1).
    surf = _pygame.surfarray.make_surface(rgb_array.swapaxes(0, 1))
    surf = _pygame.transform.scale(surf, (256 * scale, 240 * scale))
    _pg_screen.blit(surf, (0, 0))
    _pygame.display.flip()
    # Pump events so the OS doesn't mark the window unresponsive — and so
    # the user can close it with the X button to abort the run.
    for ev in _pygame.event.get():
        if ev.type == _pygame.QUIT:
            return False
    return True


def viewer_close():
    global _pygame
    if _pygame is not None:
        _pygame.quit()
        _pygame = None


def bootstrap_brief(client, model, frame, max_side=384):
    """One-shot text completion: 'what game is this, how do you play it?'

    The model writes a strategy brief that gets prepended to the system prompt
    for every subsequent decision. Keeps the loop game-agnostic — the model
    itself supplies the per-game knowledge.
    """
    img_b64 = frame_to_jpeg_b64(frame, max_side=max_side)
    resp = client.chat.completions.create(
        model=model,
        messages=[
            {"role": "system", "content": GAME_BRIEF_PROMPT},
            {"role": "user", "content": [
                {"type": "image_url",
                 "image_url": {
                    "url": f"data:image/jpeg;base64,{img_b64}",
                    "detail": "high",
                 }},
                {"type": "text",
                 "text": "Identify the game and write the strategy brief."},
            ]},
        ],
        max_tokens=600,
    )
    return resp.choices[0].message.content.strip()


def frame_to_jpeg_b64(rgb_array, max_side=512, quality=92):
    """RGB numpy frame -> base64 JPEG suitable for image_url content.

    NES native is 256x240 — each 8x8 sprite cell is only 8 pixels wide, which
    is right at the edge of what GPT-4o can spatially reason about. We
    UPSCALE with nearest-neighbor (preserves pixel boundaries) so cells are
    24-32px instead of 8px. That gives the model enough resolution to count
    columns and identify piece shapes precisely."""
    img = Image.fromarray(rgb_array)
    w, h = img.size
    longest = max(w, h)
    if longest < max_side:
        # Upscale by integer factor so pixel boundaries stay crisp.
        scale = max_side // longest
        if scale >= 2:
            img = img.resize((w * scale, h * scale), Image.NEAREST)
    elif longest > max_side:
        img.thumbnail((max_side, max_side))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality)
    return base64.b64encode(buf.getvalue()).decode("ascii")


# --- Auto-advance (zero-API intro skipper) ----------------------------------

def auto_advance(env, state, render, max_seconds=20,
                  busy_threshold=0.015, busy_window_frames=30):
    """Spam START locally until the screen looks 'busy' (suggesting active
    gameplay) or max_seconds wall-clock elapse. Burns ZERO API tokens — this
    is just for skipping boot logos, copyright screens, and 'PUSH START'
    screens that don't need VLM intelligence.

    Returns the final frame so the caller can continue from it.
    """
    start_bits = NES_BUTTON_BITS["START"]
    max_frames = 60 * max_seconds
    n = 0
    last_frame = None
    busy_window = []
    print(f"[bot] auto-advancing (no API calls) until 'busy' content or {max_seconds}s...")

    while n < max_frames:
        bits = start_bits if (n % 30) < 6 else 0
        t_frame = time.time()
        step = env.step(bits)
        if len(step) == 5:
            state, _, term, trunc, _ = step
            done = term or trunc
        else:
            state, _, done, _ = step
        n += 1
        if done:
            r = env.reset()
            state = r[0] if isinstance(r, tuple) else r

        if last_frame is not None and last_frame.shape == state.shape:
            diff_rate = float((state != last_frame).sum()) / state.size
        else:
            diff_rate = 0.0
        busy_window.append(diff_rate)
        if len(busy_window) > busy_window_frames:
            busy_window.pop(0)

        last_frame = state.copy()
        if render and not viewer_show(state):
            return state, "viewer-closed"

        elapsed = time.time() - t_frame
        if elapsed < REAL_FRAME_DT:
            time.sleep(REAL_FRAME_DT - elapsed)

        if n > 60 and len(busy_window) == busy_window_frames:
            avg = sum(busy_window) / len(busy_window)
            if avg > busy_threshold:
                print(f"[bot] busy content detected at frame {n} "
                      f"(avg diff {avg:.3f} > {busy_threshold}); engaging VLM.")
                return state, "busy"

    print(f"[bot] auto-advance hit {max_seconds}s cap at frame {n}; engaging VLM.")
    return state, "timeout"


# --- Action sequence execution ----------------------------------------------

def execute_sequence(env, state, actions, default_hold_frames, render):
    """Run a list of actions against the emulator synchronously, at 60 Hz
    real-time pacing. Updates viewer between frames. Returns the final state
    plus a status: 'ok', 'done' (env reset), or 'viewer-closed'."""
    if not actions:
        actions = [{"buttons": [], "hold_frames": 30}]
    for a in actions:
        hold_frames = max(1, min(120, int(a.get("hold_frames", default_hold_frames))))
        bits = buttons_to_action(a.get("buttons") or [])
        for _ in range(hold_frames):
            t_frame = time.time()
            step = env.step(bits)
            if len(step) == 5:
                state, _, term, trunc, _ = step
                done = term or trunc
            else:
                state, _, done, _ = step
            if render and not viewer_show(state):
                return state, "viewer-closed"
            if done:
                r = env.reset()
                state = r[0] if isinstance(r, tuple) else r
                return state, "done"
            elapsed = time.time() - t_frame
            if elapsed < REAL_FRAME_DT:
                time.sleep(REAL_FRAME_DT - elapsed)
    return state, "ok"


# --- Background emulator thread ---------------------------------------------

class GameThread:
    """The NES emulator runs continuously in this thread at 60 Hz wall clock,
    reading the currently-held button bits from a shared variable. The main
    thread sets `.bits` to press/release buttons and reads `.frame` to get
    the latest rendered frame. This way the game keeps advancing even while
    the OpenAI API call is in flight — the window never freezes."""

    def __init__(self, env):
        self.env = env
        self._lock = threading.Lock()
        self._frame_lock = threading.Lock()
        self._bits = 0
        self._frame = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        r = self.env.reset()
        s = r[0] if isinstance(r, tuple) else r
        with self._frame_lock:
            self._frame = s.copy()
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=1.0)

    def set_bits(self, b):
        with self._lock:
            self._bits = b

    def get_frame(self):
        with self._frame_lock:
            return None if self._frame is None else self._frame.copy()

    def _run(self):
        while not self._stop.is_set():
            t = time.time()
            with self._lock:
                b = self._bits
            step = self.env.step(b)
            if len(step) == 5:
                state, _, term, trunc, _ = step
                done = term or trunc
            else:
                state, _, done, _ = step
            if done:
                r = self.env.reset()
                state = r[0] if isinstance(r, tuple) else r
            with self._frame_lock:
                self._frame = state.copy()
            elapsed = time.time() - t
            if elapsed < REAL_FRAME_DT:
                time.sleep(REAL_FRAME_DT - elapsed)


def _viewer_pump(gt, render):
    """Render the latest frame and pump pygame events. Returns False if the
    user closed the window. Used in tight loops while we wait on the API."""
    if not render:
        return True
    frame = gt.get_frame()
    if frame is None:
        return True
    return viewer_show(frame)


def auto_advance_threaded(gt, render, max_seconds, busy_threshold=0.015,
                           busy_window_frames=30):
    """Press START via the GameThread until the screen looks 'busy'. Burns
    zero API tokens. Equivalent to auto_advance() but driving the background
    emulator thread instead of stepping the env directly."""
    start_bits = NES_BUTTON_BITS["START"]
    deadline = time.time() + max_seconds
    last_frame = None
    busy_window = []
    n = 0
    print(f"[bot] auto-advancing (no API calls) until 'busy' content or {max_seconds}s...")
    while time.time() < deadline:
        # Press START for the first 6 frames of every 30-frame cycle.
        bits = start_bits if (n % 30) < 6 else 0
        gt.set_bits(bits)
        time.sleep(REAL_FRAME_DT)
        if not _viewer_pump(gt, render):
            return "viewer-closed"
        frame = gt.get_frame()
        if frame is None:
            continue
        n += 1
        if last_frame is not None and last_frame.shape == frame.shape:
            d = float((frame != last_frame).sum()) / frame.size
        else:
            d = 0.0
        busy_window.append(d)
        if len(busy_window) > busy_window_frames:
            busy_window.pop(0)
        last_frame = frame
        if n > 60 and len(busy_window) == busy_window_frames:
            avg = sum(busy_window) / len(busy_window)
            if avg > busy_threshold:
                print(f"[bot] busy content detected at step {n} "
                      f"(avg diff {avg:.3f}); engaging VLM.")
                gt.set_bits(0)
                return "busy"
    gt.set_bits(0)
    print(f"[bot] auto-advance hit {max_seconds}s cap; engaging VLM.")
    return "timeout"


def call_play_api(client, model, system_prompt, img_b64, ctx_text, timeout_s=30.0):
    """Single chat-completions call. Returns (actions, screen, goal,
    knowledge, elapsed_ms). Exceptions become empty action lists so the
    caller can keep looping. Hard timeout so a hung connection doesn't
    freeze the bot."""
    t0 = time.time()
    actions, screen, goal, knowledge = [], "", "", ""
    try:
        resp = client.chat.completions.create(
            model=model,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": [
                    {"type": "image_url",
                     "image_url": {
                        "url": f"data:image/jpeg;base64,{img_b64}",
                        "detail": "high",
                     }},
                    {"type": "text", "text": ctx_text},
                ]},
            ],
            tools=TOOLS,
            tool_choice={"type": "function", "function": {"name": "play"}},
            max_tokens=900,
            timeout=timeout_s,
        )
        msg = resp.choices[0].message
        if msg.tool_calls:
            tc = msg.tool_calls[0]
            try:
                ca = json.loads(tc.function.arguments)
            except json.JSONDecodeError:
                ca = {}
            screen = (ca.get("screen") or "")[:140]
            goal = (ca.get("goal") or "")[:140]
            knowledge = (ca.get("knowledge") or "")[:1200]
            actions = ca.get("actions") or []
        else:
            screen = "(no tool call)"
            goal = (msg.content or "")[:140]
    except Exception as e:
        screen = "(error)"
        goal = str(e)[:140]
    return actions, screen, goal, knowledge, int((time.time() - t0) * 1000)


# --- Main loop --------------------------------------------------------------

def run(args):
    # Line-buffer stdout so we see logs scroll live.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    env = NESEnv(args.rom)
    if args.render:
        viewer_init(title=f"joypad-bot — {Path(args.rom).name}")
    save_dir = Path(args.save_frames) if args.save_frames else None
    if save_dir:
        save_dir.mkdir(parents=True, exist_ok=True)

    # Start the background emulator. From this point on, the game ticks at
    # 60 Hz continuously; we never block it on the API.
    gt = GameThread(env)
    gt.start()
    # Wait one tick so the first frame is ready.
    while gt.get_frame() is None:
        time.sleep(REAL_FRAME_DT)

    # Auto-advance: spam START in the GameThread while rendering — the
    # window stays smooth because the emulator is still ticking.
    if args.auto_advance_seconds > 0:
        outcome = auto_advance_threaded(gt, args.render,
                                         args.auto_advance_seconds)
        if outcome == "viewer-closed":
            gt.stop(); env.close(); viewer_close()
            return
    post_intro = gt.get_frame()
    if save_dir:
        Image.fromarray(post_intro).save(save_dir / "post_intro.png")

    # Bootstrap brief — identifies the game from the post-intro frame.
    print(f"[bot] bootstrapping — identifying the game with {args.brief_model}...")
    client = OpenAI()
    t0 = time.time()
    brief = bootstrap_brief(client, args.brief_model, post_intro,
                             max_side=args.max_side)
    print(f"[bot] brief ({int((time.time()-t0)*1000)} ms):")
    print("------ STRATEGY BRIEF ------")
    print(brief)
    print("------ END BRIEF ------\n")
    if save_dir:
        (save_dir / "brief.txt").write_text(brief)

    system_prompt = (
        f"{BASE_SYSTEM_PROMPT}\n\n"
        f"--- GAME STRATEGY BRIEF (from initial scouting) ---\n{brief}\n"
        f"--- END BRIEF ---"
    )

    history = []
    prev_state = None
    prev_call = None
    gameplay_locked = False
    busy_streak = 0
    no_change_streak = 0
    knowledge = ""   # persistent across turns; the model rewrites it each turn
    api_executor = ThreadPoolExecutor(max_workers=1)

    print(f"[bot] playing with {args.model} — max {args.max_decisions} decisions. "
          f"(emulator runs continuously; window won't freeze during API calls.)\n")

    i = 0
    try:
        while args.max_decisions == 0 or i < args.max_decisions:
            cur_state = gt.get_frame()
            if cur_state is None:
                time.sleep(REAL_FRAME_DT)
                continue

            # Diff vs previous decision's snapshot.
            if prev_state is not None and prev_state.shape == cur_state.shape:
                unchanged = bool((prev_state == cur_state).all())
                diff_rate = float((prev_state != cur_state).sum()) / cur_state.size
            else:
                unchanged = False
                diff_rate = 0.0

            # Require a higher threshold (>2% pixel change) AND a longer
            # streak (5 decisions) so the lock doesn't fire on menu fade-in
            # or single-piece animations — only sustained gameplay-like motion.
            if diff_rate > 0.02:
                busy_streak += 1
                if busy_streak >= 5 and not gameplay_locked:
                    gameplay_locked = True
                    print("[bot] gameplay-mode lock engaged — START/SELECT "
                          "will be stripped from action sequences.")
            else:
                busy_streak = 0

            # Track no-change streak. If we get stuck on a screen that needs
            # START to advance (a menu or paused state), the lock would
            # otherwise keep stripping START forever. Override the lock once
            # we've seen no-change for several decisions.
            if unchanged:
                no_change_streak += 1
            else:
                no_change_streak = 0
            unstuck_override = gameplay_locked and no_change_streak >= 3

            lock_note = (" GAMEPLAY-MODE LOCK is ON — START/SELECT are forbidden."
                         if gameplay_locked else "")
            knowledge_block = (f"\n\nYOUR ACCUMULATED KNOWLEDGE (rewrite + extend "
                                f"this in the 'knowledge' field of your next "
                                f"tool call so it persists):\n{knowledge}"
                                if knowledge else
                                "\n\nYou have no accumulated knowledge yet. "
                                "Treat early turns as EXPERIMENTATION: use "
                                "short exploratory presses and record what "
                                "you observe in the 'knowledge' field.")
            if prev_call is not None:
                prev_seq = " -> ".join(
                    f"({','.join(a.get('buttons') or []) or 'wait'},"
                    f"{a.get('hold_frames', args.default_hold_frames)}f)"
                    for a in (prev_call.get('actions') or [])
                ) or "(empty)"
                ctx = (
                    f"Decision {i}. Previous goal: {prev_call.get('goal','')}. "
                    f"Previous sequence: {prev_seq}. The game ran continuously "
                    f"while you were thinking — the current frame reflects "
                    f"~{prev_call.get('elapsed_ms', 1500)} ms of additional "
                    f"emulator time since your last decision.{lock_note} "
                    f"Compare the current frame to your previous goal to "
                    f"verify what your inputs ACTUALLY did, and update your "
                    f"knowledge accordingly.{knowledge_block}"
                )
            else:
                ctx = f"Decision {i}. First frame.{lock_note} Play.{knowledge_block}"

            img_b64 = frame_to_jpeg_b64(cur_state, max_side=args.max_side)
            if save_dir:
                Image.fromarray(cur_state).save(save_dir / f"{i:04d}_sent.png")

            # Fire the API call in a worker thread so we can keep the
            # viewer alive while we wait. The game thread is also still
            # ticking — so the screen keeps animating with no input.
            future = api_executor.submit(
                call_play_api, client, args.model, system_prompt, img_b64, ctx)
            while not future.done():
                if args.render and not _viewer_pump(gt, args.render):
                    print("[bot] viewer closed — exiting.")
                    future.cancel(); gt.stop(); env.close(); viewer_close()
                    return
                time.sleep(REAL_FRAME_DT)
            actions, screen, goal, new_knowledge, elapsed_ms = future.result()
            # Persist knowledge across turns. If the model returned no
            # knowledge (e.g. API error), keep the prior knowledge intact.
            if new_knowledge.strip():
                knowledge = new_knowledge

            # Decide whether to strip START/SELECT. We let them through if:
            #   (a) the model's own description says the screen is paused
            #       (so a single START is needed to unpause), OR
            #   (b) we've been stuck on the same screen for 3+ decisions
            #       (probably a menu the model needs to advance).
            # Otherwise, if gameplay is locked, strip them.
            screen_lower = screen.lower()
            paused = ("paus" in screen_lower)
            allow_start = paused or unstuck_override
            if gameplay_locked and actions and not allow_start:
                stripped = False
                for a in actions:
                    btns = [b for b in (a.get("buttons") or [])
                            if b not in ("START", "SELECT")]
                    if btns != (a.get("buttons") or []):
                        stripped = True
                    a["buttons"] = btns
                if stripped:
                    print(f"[{i:03d}] stripped START/SELECT from sequence.")
            elif allow_start and gameplay_locked:
                why = "paused screen" if paused else f"no-change streak {no_change_streak}"
                print(f"[{i:03d}] {why} — allowing START/SELECT through.")

            diff_tag = "  [no-change]" if (prev_call is not None and unchanged) else ""
            diff_pct = f"  diff={diff_rate*100:.1f}%"
            phase = "GP " if gameplay_locked else "   "
            print(f"[{i:03d}] {phase} {elapsed_ms:4d}ms  screen: {screen}{diff_tag}{diff_pct}")
            print(f"        goal:   {goal}")
            print(f"        seq:    " + (
                " -> ".join(
                    f"({','.join(a.get('buttons') or []) or 'wait'},"
                    f"{a.get('hold_frames', args.default_hold_frames)}f)"
                    for a in actions
                ) or "(empty)"
            ))
            if new_knowledge.strip():
                # Print just the first 180 chars per turn so the log stays
                # readable; the full knowledge is saved in history.json.
                short = new_knowledge.replace("\n", " ")[:180]
                print(f"        know:   {short}{'...' if len(new_knowledge) > 180 else ''}")

            history.append({
                "decision": i, "ms": elapsed_ms, "screen": screen,
                "goal": goal, "actions": actions, "knowledge": new_knowledge,
                "diff_rate": diff_rate, "gameplay_locked": gameplay_locked,
                "prev_action_had_effect": (prev_call is not None and not unchanged)
                                          if prev_call else None,
            })

            prev_state = cur_state
            prev_call = {"screen": screen, "goal": goal,
                         "actions": actions, "elapsed_ms": elapsed_ms}

            # Execute the sequence by telling the GameThread which buttons
            # to hold for how long, then advancing real time while it runs.
            for a in (actions or [{"buttons": [], "hold_frames": 6}]):
                hold_frames = max(1, min(120, int(a.get("hold_frames",
                                                        args.default_hold_frames))))
                bits = buttons_to_action(a.get("buttons") or [])
                gt.set_bits(bits)
                # Sleep for the hold duration while pumping the viewer.
                deadline = time.time() + hold_frames * REAL_FRAME_DT
                while time.time() < deadline:
                    if args.render and not _viewer_pump(gt, args.render):
                        print("[bot] viewer closed — exiting.")
                        gt.stop(); env.close(); viewer_close()
                        return
                    time.sleep(REAL_FRAME_DT)
            gt.set_bits(0)
            i += 1
    except KeyboardInterrupt:
        print("\n[bot] interrupted.")
    finally:
        gt.stop()
        env.close()
        viewer_close()

    if save_dir and history:
        with (save_dir / "history.json").open("w") as f:
            json.dump(history, f, indent=2)

    if history:
        n = len(history)
        mean_ms = sum(h["ms"] for h in history) / n
        p95_ms = sorted(h["ms"] for h in history)[int(n * 0.95)]
        print(f"\n[bot] {n} decisions  mean {mean_ms:.0f} ms  p95 {p95_ms} ms")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, help="Path to an .nes ROM.")
    p.add_argument("--model", default="gpt-4o",
                   help="OpenAI vision-capable chat-completion model. "
                        "gpt-4o is the default; gpt-4o-mini is ~10x cheaper "
                        "but worse at multi-step reasoning.")
    p.add_argument("--brief-model", default="gpt-4o",
                   help="Model for the one-shot game-identification brief.")
    p.add_argument("--default-hold-frames", type=int, default=6,
                   help="Default hold_frames for actions that omit it (60 fps base).")
    p.add_argument("--max-decisions", type=int, default=200,
                   help="Stop after this many decisions. 0 = run forever.")
    p.add_argument("--auto-advance-seconds", type=int, default=15,
                   help="Spam START locally for up to this many seconds "
                        "before any API calls, to skip boot/title/intro "
                        "screens without burning tokens. 0 disables.")
    p.add_argument("--render", action="store_true",
                   help="Open a pygame window to watch the agent play.")
    p.add_argument("--save-frames", default=None,
                   help="Directory to save frames + history.json.")
    p.add_argument("--max-side", type=int, default=512,
                   help="Target longest-side for frames sent to the model. "
                        "NES native is 256x240; we upscale with nearest-"
                        "neighbor to this size so per-cell pixels are large "
                        "enough for GPT-4o to count reliably. 512 = 2x "
                        "upscale (cells become 16x16) and stays under "
                        "gpt-4o TPM limits at 1 request/sec.")
    args = p.parse_args()

    if not os.environ.get("OPENAI_API_KEY"):
        sys.exit("error: OPENAI_API_KEY env var is required")
    if not Path(args.rom).is_file():
        sys.exit(f"error: ROM not found: {args.rom}")

    run(args)


if __name__ == "__main__":
    main()
