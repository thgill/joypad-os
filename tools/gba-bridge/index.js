#!/usr/bin/env node
// joypad-gba-bridge — host-side daemon driving raw joybus over USB CDC.
//
// Architecture: see ../../.dev/docs/dolphin-gba-bridge.md
//
// What used to be GBA-specific firmware logic (Kawasedo cipher,
// multiboot state machine, RESET-then-poll-PSF0, retry timing) now
// lives entirely here. The firmware exposes only:
//
//   JOYBUS.BRIDGE.START / STOP — bus ownership
//   JOYBUS.XFER {tx, rx_len, timeout_us} — primitive bus exchange
//
// Iterating timing/retries here means no firmware reflash per change.
//
// Usage:
//   node index.js --probe-only              # quick STATUS handshake
//   node index.js --multiboot rom.gba       # upload a multiboot ROM
//   node index.js --reset                   # joybus RESET (kicks running payload)
//   node index.js                           # (eventually) start TCP listener

import { SerialPort } from 'serialport';
import { readFileSync } from 'node:fs';
import { argv, exit } from 'node:process';
import net from 'node:net';

// ---------- CDC framing (matches src/usb/usbd/cdc/cdc_protocol.h) ----------
const SYNC = 0xAA, TYPE_CMD = 0x01, TYPE_RSP = 0x02;

function crc16(bytes, init = 0xFFFF, poly = 0x1021) {
  let crc = init;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ poly) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
  }
  return crc;
}
function frame(type, seq, payload) {
  const buf = Buffer.alloc(1 + 2 + 1 + 1 + payload.length + 2);
  buf[0] = SYNC;
  buf.writeUInt16LE(payload.length, 1);
  buf[3] = type;
  buf[4] = seq;
  payload.copy(buf, 5);
  buf.writeUInt16LE(crc16(Buffer.concat([Buffer.from([type, seq]), payload])), 5 + payload.length);
  return buf;
}

// ---------- Port discovery ----------
async function findGc2UsbPort() {
  const ports = await SerialPort.list();
  // Filter out the KB2040 UART bridge typically at usbmodem1101.
  const candidates = ports.filter(p =>
    p.path.includes('usbmodem') && !p.path.endsWith('1101'));
  if (!candidates.length) throw new Error('No GC2USB CDC port found.');
  if (candidates.length > 1) console.warn('[bridge] multiple candidates, using:', candidates[0].path);
  return candidates[0].path;
}

// ---------- CDC client ----------
class CdcClient {
  constructor(path) {
    this.port = new SerialPort({ path, baudRate: 115200 });
    this.seq = 0;
    this.rxBuf = Buffer.alloc(0);
    this.pending = null;
    this.port.on('data', (c) => this._onData(c));
  }
  _onData(chunk) {
    this.rxBuf = Buffer.concat([this.rxBuf, chunk]);
    while (this.rxBuf.length >= 6) {
      if (this.rxBuf[0] !== SYNC) {
        const i = this.rxBuf.indexOf(SYNC);
        if (i < 0) { this.rxBuf = Buffer.alloc(0); return; }
        this.rxBuf = this.rxBuf.subarray(i);
        continue;
      }
      const len = this.rxBuf.readUInt16LE(1);
      const total = 7 + len;
      if (this.rxBuf.length < total) return;
      const type = this.rxBuf[3], seq = this.rxBuf[4];
      const payload = this.rxBuf.subarray(5, 5 + len);
      this.rxBuf = this.rxBuf.subarray(total);
      if (type === TYPE_RSP && this.pending && seq === this.pending.seq) {
        clearTimeout(this.pending.timer);
        const p = this.pending; this.pending = null;
        try { p.resolve(JSON.parse(payload.toString('utf8'))); }
        catch (e) { p.reject(e); }
      }
    }
  }
  cmd(name, args = {}, timeoutMs = 3000) {
    return new Promise((resolve, reject) => {
      if (this.pending) return reject(new Error('cmd already in flight'));
      const seq = this.seq = (this.seq + 1) & 0xFF;
      const payload = Buffer.from(JSON.stringify({ cmd: name, ...args }), 'utf8');
      const timer = setTimeout(() => {
        this.pending = null;
        reject(new Error(`timeout: ${name}`));
      }, timeoutMs);
      this.pending = { resolve, reject, seq, timer };
      this.port.write(frame(TYPE_CMD, seq, payload));
    });
  }
  close() { return new Promise(r => this.port.close(r)); }
}

// ---------- joybus xfer wrapper ----------
async function jbXfer(cdc, tx, rxLen = 0, timeoutUs = 1000) {
  const txHex = Buffer.from(tx).toString('hex');
  const r = await cdc.cmd('JOYBUS.XFER', { tx: txHex, rx_len: rxLen, timeout_us: timeoutUs });
  if (!r.ok) {
    const err = new Error(`JOYBUS.XFER failed: ${r.error || 'unknown'} (code ${r.code})`);
    err.code = r.code;
    throw err;
  }
  return Buffer.from(r.rx, 'hex');
}

// ---------- GBA multiboot constants (mirror gba_multiboot.c) ----------
const JSTAT_PSF1 = 0x20, JSTAT_PSF0 = 0x10, JSTAT_SEND = 0x08, JSTAT_RECV = 0x02;
const JSTAT_VALID_MASK = 0xC5;
const GBA_TYPE_ID = 0x0400;
const MAGIC_SEDO = 0x6f646573, MAGIC_KAWA = 0x6177614b, MAGIC_BY = 0x20796220;
const CRC_POLY = 0xa1c1, CRC_SEED = 0x15a0;
const GBA_DELAY_US = 70;

// Kawasedo session-key derivation (palette=3, speed=0 hardcoded).
function calculateGcKey(romLen) {
  const size = (romLen - 0x200) >>> 3;
  let res1 = (size & 0x3F80) << 1;
  res1 |= (size & 0x4000) << 2;
  res1 |= (size & 0x7F);
  res1 |= 0x380000;
  res1 = res1 >>> 0;
  let res2 = (res1 >>> 8) >>> 0;
  res2 = (res2 + (res1 >>> 16)) >>> 0;
  res2 = (res2 + res1) >>> 0;
  res2 = ((res2 << 24) >>> 0);
  res2 = (res2 | res1) >>> 0;
  res2 = (res2 | 0x80808080) >>> 0;
  if ((res2 & 0x200) === 0) res2 = (res2 ^ MAGIC_KAWA) >>> 0;
  else                       res2 = (res2 ^ MAGIC_SEDO) >>> 0;
  return res2;
}

function crcStep(crc, value) {
  for (let i = 0; i < 32; i++) {
    if (((crc ^ value) & 1) !== 0) crc = ((crc >>> 1) ^ CRC_POLY) >>> 0;
    else                            crc = (crc >>> 1) >>> 0;
    value = (value >>> 1) >>> 0;
  }
  return crc >>> 0;
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// Encode a batch of {tx: Buffer, rxLen: int} ops into the firmware's
// hex format and dispatch as a single CDC roundtrip. Returns array of
// {err: int, rx: Buffer} (err=0 on success, abs(error code) otherwise).
//
// Each request CDC payload is bound by CDC_MAX_PAYLOAD (1024 bytes).
// For 5-byte tx + 1-byte rx ops (the multiboot WRITE shape), each op
// encodes to 14 hex chars = ~70 ops/batch, ~135ms USB cost amortized
// over those 70 joybus exchanges.
async function jbBatch(cdc, ops, timeoutUs = 5000) {
  // Encode ops: per op = tx_len(1) + tx + rx_len(1)
  const parts = [];
  for (const op of ops) {
    const tx = Buffer.from(op.tx);
    parts.push(Buffer.from([tx.length]));
    parts.push(tx);
    parts.push(Buffer.from([op.rxLen | 0]));
  }
  const rawHex = Buffer.concat(parts).toString('hex');
  const r = await cdc.cmd('JOYBUS.BATCH',
    { ops: rawHex, timeout_us: timeoutUs }, 10000);
  if (!r.ok) throw new Error(`JOYBUS.BATCH failed: ${r.error || 'unknown'}`);
  const outBuf = Buffer.from(r.out, 'hex');

  // Decode results: per result = err(1) + got(1) + rx[got]
  const results = [];
  let off = 0;
  while (off < outBuf.length) {
    if (off + 2 > outBuf.length) break;
    const err = outBuf[off++];
    const got = outBuf[off++];
    if (off + got > outBuf.length) break;
    results.push({ err, rx: outBuf.subarray(off, off + got) });
    off += got;
  }
  return results;
}

// jb_xfer wrappers for the multiboot primitives (STATUS, READ, WRITE).
async function jbStatus(cdc, cmdByte) {
  // 1-byte cmd → 3-byte response (type lo, type hi, jstat)
  return await jbXfer(cdc, [cmdByte], 3, 5000);
}
async function jbRead4(cdc) {
  // 0x14 cmd → 5-byte response (4 data + jstat)
  return await jbXfer(cdc, [0x14], 5, 5000);
}
async function jbWrite4(cdc, w) {
  // 0x15 cmd + 4-byte word → 1-byte jstat
  const tx = Buffer.from([0x15, w & 0xFF, (w >>> 8) & 0xFF, (w >>> 16) & 0xFF, (w >>> 24) & 0xFF]);
  return await jbXfer(cdc, tx, 1, 5000);
}

// jbWrite4 with the same recovery path the original C upload uses:
// on a WRITE timeout, peek JSTAT — if RECV is set, the GBA actually
// received our data and only the ack was lost (treat as success).
// Otherwise retry the WRITE once.
async function jbWrite4Robust(cdc, w, label, idx) {
  try {
    const r = await jbWrite4(cdc, w);
    return r[0];  // jstat
  } catch (e) {
    if (e.code !== -3) throw e;  // not a timeout — propagate
  }
  // Timeout. Peek JSTAT.
  try {
    const r3 = await jbXfer(cdc, [0x00], 3, 5000);
    if (r3.length === 3 && (r3[2] & JSTAT_RECV)) {
      return r3[2];  // GBA processed it; only the ack got lost
    }
  } catch (_) { /* peek also failed — fall through to retry */ }
  // Retry once.
  try {
    const r = await jbWrite4(cdc, w);
    return r[0];
  } catch (e) {
    throw new Error(`${label} WRITE failed at i=${idx} (code ${e.code})`);
  }
}

// ---------- multiboot upload — staged (firmware-native upload) ----------
//
// Earlier iterations tried to drive the entire Kawasedo handshake from JS
// over JOYBUS.XFER/BATCH primitives. Cipher math is correct (verified
// against a Python reference), but the inter-batch CDC roundtrip pause
// (~2 ms) interrupts the BIOS's tight inter-WRITE timing tolerance and
// it silently rejects the upload at CRC time. Hybrid is the answer:
// stage the ROM into firmware first via GBA.MB.CHUNK, then trigger
// firmware-native gba_mb_upload (which has the right sleep_us(70)
// per-WRITE timing) via GBA.MB.UPLOAD.
//
// Other joybus traffic (Dolphin live link, controller polling) keeps
// using JOYBUS.XFER — multiboot's the only flow with timing this tight.
async function multibootUpload(cdc, romPath) {
  const rom = readFileSync(romPath);
  if (rom.length < 0x200 || rom.length >= 0x40000) throw new Error('ROM size out of range');
  if (rom[0xac] === 0) throw new Error('ROM[0xac] is 0 (invalid)');
  console.log(`[bridge] staging ${romPath} (${rom.length} bytes)`);

  const start = await cdc.cmd('JOYBUS.BRIDGE.START');
  if (!start.ok) throw new Error(`BRIDGE.START failed: ${start.error}`);

  try {
    // 1. Send joybus RESET — kicks our gba/joypad payload's ResetHalt
    //    into SVC 0x26 if it's running. No-op if GBA is already in BIOS.
    console.log('[bridge] RESET kick…');
    try { await jbXfer(cdc, [0xFF], 3, 5000); } catch (_) { /* ignore */ }

    // 2. Stream the ROM into firmware via GBA.MB.CHUNK. Each chunk is
    //    ~480 bytes, hex-encoded into a JSON CMD payload (well under
    //    1024-byte CDC limit). For 16-64 KB ROMs this takes ~0.2-1 s
    //    of CDC traffic — slow but only happens once, before the
    //    timing-sensitive upload begins.
    await cdc.cmd('GBA.MB.RESET');
    const CHUNK = 480;
    const stageStart = Date.now();
    for (let off = 0; off < rom.length; off += CHUNK) {
      const slice = rom.subarray(off, Math.min(off + CHUNK, rom.length));
      const r = await cdc.cmd('GBA.MB.CHUNK', { data: slice.toString('hex') });
      if (!r.ok) throw new Error(`GBA.MB.CHUNK at off=${off} failed: ${r.error}`);
    }
    console.log(`[bridge] staged ${rom.length} bytes in ${Math.round((Date.now()-stageStart)/100)/10}s`);

    // 3. Trigger firmware-native gba_mb_upload. Same code path autoboot
    //    uses — has the right sleep_us(70) inter-WRITE timing the BIOS
    //    needs. Returns when the upload completes or times out
    //    (~6-12 s wall clock). The 60 s CDC timeout covers it.
    console.log('[bridge] firing GBA.MB.UPLOAD (firmware-native)…');
    const uploadStart = Date.now();
    const r = await cdc.cmd('GBA.MB.UPLOAD', { channel: 0 }, 60000);
    const dur = Math.round((Date.now() - uploadStart) / 100) / 10;
    if (!r.ok) {
      // gba_mb_result_t error codes (negative; abs()ed in firmware):
      //   1 PROBE, 2 RESET, 3 TRANSFER, 4 FINALIZE, 5 PARAMS, 6 NO_PAYLOAD
      //   100 not active, 101 no buffer, 102 port not init
      const map = {
        1: 'PROBE — GBA not in BIOS multiboot wait state',
        2: 'RESET — STATUS handshake mismatch',
        3: 'TRANSFER — JOYSTAT validation failed mid-upload',
        4: 'FINALIZE — post-upload boot poll / CRC echo failed',
        5: 'PARAMS — bad ROM length / format',
      };
      throw new Error(`GBA.MB.UPLOAD failed (code ${r.code}: ${map[Math.abs(r.code)] || 'unknown'})`);
    }
    console.log(`[bridge] ✓ multiboot complete in ${dur}s`);
  } finally {
    await cdc.cmd('JOYBUS.BRIDGE.STOP');
  }
}

// ---------- Dolphin GBA TCP bridge ----------
//
// Protocol (from Dolphin's Source/Core/Core/HW/SI/SI_DeviceGBA.cpp):
//   Dolphin listens on 127.0.0.1:0xD6BA (54970) — the external "GBA
//   emulator" (mGBA, or in our case this daemon) CONNECTS as a client.
//   Same for 0xC10C (49420) clock-sync (4-byte time-slices we just
//   discard — for cycle-accurate emulation, we have real hardware).
//
// Per joybus exchange: Dolphin sends 1-byte cmd (or 5 for WRITE 0x15 =
// cmd + 4 data), we respond with N bytes:
//   RESET (0xFF) / STATUS (0x00) → 3 bytes (type lo, hi, jstat)
//   READ (0x14)                  → 5 bytes (4 data + jstat)
//   WRITE (0x15)                 → 1 byte (jstat)
//
// JOYBUS.BRIDGE owns the joybus while connected; JOYBUS.XFER does the
// bus work. Reconnects on Dolphin restart.
const DOLPHIN_PORT = 0xD6BA;       // 54970
const DOLPHIN_CLOCK_PORT = 0xC10C; // 49420
const CMD_STATUS = 0x00, CMD_READ = 0x14, CMD_WRITE = 0x15, CMD_RESET = 0xFF;

function rxLenFor(cmd) {
  if (cmd === CMD_RESET || cmd === CMD_STATUS) return 3;
  if (cmd === CMD_READ) return 5;
  if (cmd === CMD_WRITE) return 1;
  return 1;
}

function connectWithRetry(host, port, label, onConnect, onData) {
  let sock = null;
  const tryConnect = () => {
    sock = net.createConnection({ host, port }, () => {
      console.log(`[${label}] connected to ${host}:${port}`);
      sock.setNoDelay(true);
      if (onConnect) onConnect(sock);
    });
    sock.on('data', (c) => onData && onData(sock, c));
    sock.on('error', () => {});  // expected when Dolphin not yet up
    sock.on('close', () => {
      console.log(`[${label}] disconnected; retrying in 1s…`);
      setTimeout(tryConnect, 1000);
    });
  };
  tryConnect();
}

// Reverse calculateGcKey to recover rom_len from Dolphin's our_key WRITE.
// res1 layout (lower 24 bits): size[0..6] in bits 0..6, size[7..13] in
// bits 8..14, size[14] in bit 16, plus 0x380000 OR'd in. Then |0x80808080
// then ^MAGIC. Try both MAGIC values; the right one round-trips through
// calculateGcKey.
function reverseGcKey(ourKey) {
  for (const magic of [MAGIC_KAWA, MAGIC_SEDO]) {
    const u = (ourKey ^ magic) >>> 0;
    const lower24 = u & 0xFFFFFF;
    const size = ((lower24 & 0x7F)) | (((lower24 >>> 8) & 0x7F) << 7) | (((lower24 >>> 16) & 1) << 14);
    const romLen = (size << 3) + 0x200;
    if (calculateGcKey(romLen) === ourKey) return romLen;
  }
  return -1;
}

async function startDolphinBridge(cdc) {
  console.log('[bridge] starting Dolphin GBA TCP bridge (CLIENT mode — connecting to Dolphin)');
  const start = await cdc.cmd('JOYBUS.BRIDGE.START');
  if (!start.ok) throw new Error(`BRIDGE.START failed: ${start.error}`);

  connectWithRetry('127.0.0.1', DOLPHIN_CLOCK_PORT, 'clock', null, () => {});

  // Intercept-and-replay state machine. Dolphin's per-WRITE pace is
  // ~real-time, but each round-trip costs ~2-5 ms USB latency for us;
  // over the ~13 K WRITEs of a typical multiboot, the BIOS times out
  // mid-handshake and silently rejects at CRC. Solution: detect the
  // multiboot phase, capture Dolphin's encrypted bytes locally, decrypt
  // to recover plaintext ROM, then run our existing firmware-native
  // staged upload (which has the right inter-WRITE timing).
  //
  // States:
  //   PASSTHROUGH — every cmd forwarded to GBA verbatim. Default.
  //   AWAIT_OURKEY — saw the session_key READ; next WRITE is our_key
  //     (plaintext). Capture rom_len, allocate ROM buffer.
  //   BUFFER_HEADER — collecting the 48 plaintext header WRITEs.
  //   BUFFER_BODY — collecting encrypted body WRITEs, decrypting on the fly.
  //   REPLAYING — staged upload running on firmware. Dolphin polls get
  //     fake/null responses while we wait.
  //   POST_UPLOAD — staged upload done, GBA running Madden's mini-game.
  //     Switch back to PASSTHROUGH so live joybus traffic flows.
  // States:
  //   PASSTHROUGH — every cmd forwarded to GBA verbatim. Default.
  //   AWAIT_OURKEY — saw the multiboot seed READ; next WRITE is the
  //     game's our_key (which we just discard — we'll re-derive in
  //     firmware from a fresh seed during the staged upload).
  //   CAPTURE — collecting plaintext header + decrypted body. Continues
  //     until Dolphin sends a READ (signals end of WRITE phase).
  //   REPLAYING — staged firmware-native upload running. Dolphin polls
  //     get null/canned responses while we wait.
  //
  // Why no rom_len reversal: different games use different boot
  // palette/speed constants, so calculateGcKey-style reverse doesn't
  // necessarily round-trip. Just buffer until WRITEs stop.
  const STATE = { PASSTHROUGH:0, AWAIT_OURKEY:1, CAPTURE:2, REPLAYING:3 };
  let state = STATE.PASSTHROUGH;
  let sessionKey = 0;
  let plain = null;     // recovered plaintext ROM (Buffer)
  let plainPos = 0;     // current byte offset within plain
  let inflight = false;
  let pending = Buffer.alloc(0);

  const resetCapture = () => {
    state = STATE.PASSTHROUGH;
    sessionKey = 0; plain = null; plainPos = 0;
  };

  const drain = async (sock) => {
    if (inflight) return;
    while (pending.length >= 1) {
      const cmd = pending[0];
      const txLen = (cmd === CMD_WRITE) ? 5 : 1;
      if (pending.length < txLen) return;
      const tx = pending.subarray(0, txLen);
      pending = pending.subarray(txLen);
      const rxLen = rxLenFor(cmd);
      inflight = true;
      try {
        // --- INTERCEPT LOGIC ---
        if (state === STATE.PASSTHROUGH) {
          // Forward verbatim to real GBA.
          const rx = await jbXfer(cdc, tx, rxLen, 5000);
          const out = Buffer.alloc(rxLen); rx.copy(out);
          sock.write(out);
          // Intercept-and-replay (AWAIT_OURKEY/CAPTURE/REPLAYING below)
          // is disabled — pure passthrough only. The cipher math works,
          // but Madden's BIOS-state polling looks for JSTAT.RECV bit
          // transitions between WRITEs, and faking that drain pattern
          // is fiddly enough that a real Dolphin multiboot through
          // passthrough takes minutes (~13K USB-FS roundtrips × ~3 ms).
          // Acceptable as a stepping stone to the gc2eth path; not
          // worth more effort here.
          //
          // To re-enable when the BIOS-state model is ready:
          //   if (cmd === CMD_READ && rxLen >= 5 && (rx[4] & JSTAT_PSF0)
          //       && rx.slice(0,4).readUInt32LE() !== 0) {
          //     state = STATE.AWAIT_OURKEY; ...
          //   }
        }
        else if (state === STATE.AWAIT_OURKEY) {
          if (cmd === CMD_WRITE) {
            // First WRITE = game's our_key. Discard — our staged upload
            // will read a fresh seed and derive its own our_key. Allocate
            // max-sized buffer (256 KB multiboot ceiling) — we trim later.
            plain = Buffer.alloc(0x40000);
            plainPos = 0;
            state = STATE.CAPTURE;
            console.log('[bridge] our_key WRITE seen; capturing header+body until READ');
            sock.write(Buffer.from([0x00]));
          } else {
            // Unexpected — back off
            const rx = await jbXfer(cdc, tx, rxLen, 5000);
            const out = Buffer.alloc(rxLen); rx.copy(out); sock.write(out);
          }
        }
        else if (state === STATE.CAPTURE) {
          if (cmd === CMD_WRITE) {
            // First 48 WRITEs (192 bytes, offsets 0..0xBF) are PLAINTEXT
            // header. After that, every WRITE is encrypted body.
            if (plainPos < 0xC0) {
              plain[plainPos++] = tx[1];
              plain[plainPos++] = tx[2];
              plain[plainPos++] = tx[3];
              plain[plainPos++] = tx[4];
            } else {
              const enc = (tx[1] | (tx[2] << 8) | (tx[3] << 16) | (tx[4] << 24)) >>> 0;
              const i = plainPos;   // byte offset (matches gba_multiboot's i)
              sessionKey = ((Math.imul(sessionKey, MAGIC_KAWA) + 1) >>> 0);
              let p = (enc ^ sessionKey) >>> 0;
              p = (p ^ ((~(i + (0x20 << 20)) + 1) >>> 0)) >>> 0;
              p = (p ^ MAGIC_BY) >>> 0;
              if (plainPos + 4 <= plain.length) {
                plain[plainPos++] = p & 0xFF;
                plain[plainPos++] = (p >>> 8) & 0xFF;
                plain[plainPos++] = (p >>> 16) & 0xFF;
                plain[plainPos++] = (p >>> 24) & 0xFF;
              }
            }
            sock.write(Buffer.from([0x00]));
          } else if (cmd === CMD_READ) {
            // First READ after the WRITE storm — upload phase done.
            // Last 4 plaintext bytes are the decrypted CRC word, NOT
            // part of the ROM image. Trim them.
            const romLen = plainPos - 4;
            const rom = Buffer.from(plain.subarray(0, romLen));
            console.log(`[bridge] capture done; rom_len=${romLen} (0x${romLen.toString(16)}); kicking native upload`);
            state = STATE.REPLAYING;
            sock.write(Buffer.alloc(rxLen));   // fake CRC reply
            (async () => {
              try {
                await stagedUpload(cdc, rom);
                console.log('[bridge] ✓ replay upload complete; switching back to passthrough');
              } catch (e) {
                console.error('[bridge] replay upload failed:', e.message);
              } finally {
                resetCapture();
              }
            })();
          } else {
            // STATUS/RESET mid-upload — fake an "all OK" handshake response
            const out = Buffer.alloc(rxLen);
            if (rxLen >= 3) { out[0] = 0x00; out[1] = 0x04; out[2] = 0x12; }
            sock.write(out);
          }
        }
        else if (state === STATE.REPLAYING) {
          // Upload running on firmware — keep Dolphin's polls quiet.
          // Don't touch the bus (firmware owns it).
          if (cmd === CMD_READ) {
            sock.write(Buffer.alloc(rxLen));
          } else {
            const out = Buffer.alloc(rxLen);
            if (rxLen >= 3) { out[0] = 0x00; out[1] = 0x04; out[2] = 0x12; }
            else if (rxLen === 1) { out[0] = 0x00; }
            sock.write(out);
          }
        }
      } catch (e) {
        sock.write(Buffer.alloc(rxLen));
      } finally {
        inflight = false;
      }
    }
  };

  connectWithRetry('127.0.0.1', DOLPHIN_PORT, 'joybus',
    () => { pending = Buffer.alloc(0); inflight = false; resetCapture(); },
    (sock, chunk) => {
      pending = Buffer.concat([pending, chunk]);
      drain(sock);
    });

  await new Promise(() => {});
}

// Helper: stage + upload via firmware native path. Used by the
// intercept-and-replay logic above and could also be called directly
// for non-Dolphin uploads.
async function stagedUpload(cdc, rom) {
  console.log(`[bridge] staging ${rom.length} bytes for native upload…`);
  await cdc.cmd('GBA.MB.RESET');
  const CHUNK = 480;
  for (let off = 0; off < rom.length; off += CHUNK) {
    const slice = rom.subarray(off, Math.min(off + CHUNK, rom.length));
    const r = await cdc.cmd('GBA.MB.CHUNK', { data: slice.toString('hex') });
    if (!r.ok) throw new Error(`GBA.MB.CHUNK at off=${off}: ${r.error}`);
  }
  console.log('[bridge] firing GBA.MB.UPLOAD…');
  const r = await cdc.cmd('GBA.MB.UPLOAD', { channel: 0 }, 60000);
  if (!r.ok) throw new Error(`GBA.MB.UPLOAD code=${r.code}`);
}

// ---------- CLI ----------
async function main() {
  const probeOnly = argv.includes('--probe-only');
  const resetOnly = argv.includes('--reset');
  const dolphin   = argv.includes('--dolphin');
  const mbIdx = argv.indexOf('--multiboot');
  const mbPath = mbIdx >= 0 ? argv[mbIdx + 1] : null;

  const portPath = await findGc2UsbPort();
  console.log('[bridge] opening', portPath);
  const cdc = new CdcClient(portPath);

  if (probeOnly) {
    await cdc.cmd('JOYBUS.BRIDGE.START');
    try {
      const r = await jbXfer(cdc, [0x00], 3, 5000);
      const id = r[0] | (r[1] << 8);
      console.log(`[bridge] STATUS → type=0x${id.toString(16)} jstat=0x${r[2].toString(16)}`);
      console.log(`[bridge]   PSF0=${(r[2] & JSTAT_PSF0) ? 'YES' : 'no'}`,
                  `PSF1=${(r[2] & JSTAT_PSF1) ? 'YES' : 'no'}`,
                  `SEND=${(r[2] & JSTAT_SEND) ? 'YES' : 'no'}`,
                  `RECV=${(r[2] & JSTAT_RECV) ? 'YES' : 'no'}`);
    } finally { await cdc.cmd('JOYBUS.BRIDGE.STOP'); }
    await cdc.close();
    return;
  }

  if (resetOnly) {
    await cdc.cmd('JOYBUS.BRIDGE.START');
    try {
      console.log('[bridge] sending joybus RESET (0xFF)…');
      const r = await jbXfer(cdc, [0xFF], 3, 5000);
      console.log(`[bridge] RESET → ${r.toString('hex')}`);
    } finally { await cdc.cmd('JOYBUS.BRIDGE.STOP'); }
    await cdc.close();
    return;
  }

  if (mbPath) {
    try { await multibootUpload(cdc, mbPath); }
    finally { await cdc.close(); }
    return;
  }

  if (dolphin) {
    // Runs forever serving Dolphin's GBA TCP. Ctrl-C to stop.
    process.on('SIGINT', async () => {
      console.log('\n[bridge] shutting down…');
      try { await cdc.cmd('JOYBUS.BRIDGE.STOP'); } catch (_) {}
      await cdc.close();
      exit(0);
    });
    await startDolphinBridge(cdc);
    return;
  }

  console.log('[bridge] usage:');
  console.log('  node index.js --probe-only');
  console.log('  node index.js --reset');
  console.log('  node index.js --multiboot rom.gba');
  console.log('  node index.js --dolphin              # serve Dolphin GBA TCP');
  await cdc.close();
}

main().catch(e => { console.error(e); exit(1); });
