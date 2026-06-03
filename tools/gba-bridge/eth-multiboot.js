#!/usr/bin/env node
// eth-multiboot.js — fake Dolphin GBA-link cable for gc2eth.
//
// Listens on TCP 54970, waits for gc2eth's CH9120 to connect (it's in
// TCP_CLIENT mode connecting to us), then drives the full Kawasedo
// multiboot protocol verbatim from gba_multiboot.c. The bridge faithfully
// forwards every joybus command to the GBA on GP2.
//
// Goal: prove the bridge (TCP <-> CH9120 <-> UART <-> joybus <-> GBA) is
// correct end-to-end, without involving real Dolphin or any specific game.
// If the GBA boots the .gba ROM passed via --rom, the bridge works.
//
// Usage:
//   node eth-multiboot.js --rom /tmp/joypad_mb.gba

import { createServer } from 'node:net';
import { readFileSync } from 'node:fs';

// ============================================================================
// Kawasedo constants (mirror src/native/host/gc/gba_multiboot.c)
// ============================================================================
const JSTAT_PSF0 = 0x10;
const JSTAT_RECV = 0x02;
const JSTAT_SEND = 0x08;
const JSTAT_VALID_MASK = 0xC5;
const GBA_TYPE_ID = 0x0400;
const MAGIC_SEDO = 0x6f646573;
const MAGIC_KAWA = 0x6177614b;
const MAGIC_BY   = 0x20796220;
const CRC_POLY   = 0xa1c1;
const CRC_SEED   = 0x15a0;

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
    value = value >>> 1;
  }
  return crc;
}

// ============================================================================
// TCP socket helpers — send N bytes, wait for N response bytes.
// CH9120 may split responses across TCP packets; we accumulate until we
// have the expected count.
// ============================================================================
class TcpJoybus {
  constructor(sock) {
    this.sock = sock;
    this.rxQueue = Buffer.alloc(0);
    this.waiters = [];
    sock.on('data', (chunk) => {
      this.rxQueue = Buffer.concat([this.rxQueue, chunk]);
      this._drain();
    });
  }
  _drain() {
    while (this.waiters.length > 0 && this.rxQueue.length >= this.waiters[0].n) {
      const w = this.waiters.shift();
      const out = this.rxQueue.subarray(0, w.n);
      this.rxQueue = this.rxQueue.subarray(w.n);
      w.resolve(out);
    }
  }
  recv(n, timeoutMs = 2000) {
    return new Promise((resolve, reject) => {
      const w = { n, resolve, reject };
      this.waiters.push(w);
      this._drain();
      setTimeout(() => {
        const i = this.waiters.indexOf(w);
        if (i >= 0) {
          this.waiters.splice(i, 1);
          reject(new Error(`recv(${n}) timeout after ${timeoutMs}ms (queue has ${this.rxQueue.length} bytes)`));
        }
      }, timeoutMs);
    });
  }
  send(buf) {
    return new Promise((resolve) => {
      this.sock.write(buf, () => resolve());
    });
  }
  async xfer(tx, rxLen) {
    await this.send(tx);
    return this.recv(rxLen);
  }
}

// ============================================================================
// Multiboot driver
// ============================================================================
async function multiboot(jb, rom, channel = 0) {
  console.log(`[mb] starting multiboot of ${rom.length} bytes (${rom.length.toString(16)}h)`);

  // Step 1: RESET handshake. Expect type=0x0400 + jstat from a GBA in
  // BIOS multiboot wait state.
  console.log('[mb] step 1: RESET');
  const rst = await jb.xfer(Buffer.from([0xFF]), 3);
  console.log(`[mb]   rx ${rst.toString('hex')}`);
  const rstType = rst[0] | (rst[1] << 8);
  if (rstType !== GBA_TYPE_ID) throw new Error(`expected GBA type 0x0400, got 0x${rstType.toString(16)}`);

  await sleep(10);

  // Step 2: STATUS — confirm PSF0 (BIOS multiboot ready).
  console.log('[mb] step 2: STATUS (poll for PSF0)');
  let statusJstat = 0;
  for (let i = 0; i < 50; i++) {
    const r = await jb.xfer(Buffer.from([0x00]), 3);
    const type = r[0] | (r[1] << 8);
    statusJstat = r[2];
    if (type === GBA_TYPE_ID && (statusJstat & JSTAT_PSF0)) {
      console.log(`[mb]   PSF0 set on poll ${i}: rx=${r.toString('hex')}`);
      break;
    }
    await sleep(20);
  }
  if (!(statusJstat & JSTAT_PSF0)) throw new Error('PSF0 never set');

  await sleep(1);

  // Step 3: READ session_key seed.
  console.log('[mb] step 3: READ session_key');
  let sessionKey = 0;
  for (let i = 0; i < 50; i++) {
    const r = await jb.xfer(Buffer.from([0x14]), 5);
    sessionKey = (r[0] | (r[1] << 8) | (r[2] << 16) | (r[3] << 24)) >>> 0;
    if (sessionKey !== 0) {
      console.log(`[mb]   seed=0x${sessionKey.toString(16).padStart(8, '0')} jstat=0x${r[4].toString(16).padStart(2, '0')} (poll ${i})`);
      break;
    }
    await sleep(5);
  }
  if (sessionKey === 0) throw new Error('session_key stayed 0');
  sessionKey = (sessionKey ^ MAGIC_SEDO) >>> 0;
  await sleep(2);

  // Step 4: WRITE our_key (derived from rom length).
  const ourKey = calculateGcKey(rom.length);
  console.log(`[mb] step 4: WRITE our_key=0x${ourKey.toString(16).padStart(8, '0')}`);
  {
    const cmd = Buffer.from([0x15, ourKey & 0xff, (ourKey >> 8) & 0xff, (ourKey >> 16) & 0xff, (ourKey >> 24) & 0xff]);
    const r = await jb.xfer(cmd, 1);
    if (r[0] & JSTAT_VALID_MASK) throw new Error(`our_key bad jstat 0x${r[0].toString(16)}`);
  }

  // Step 5: stream UNENCRYPTED header (0..0xBF).
  console.log('[mb] step 5: streaming header (192 bytes, plaintext)');
  for (let i = 0; i < 0xC0; i += 4) {
    const word = rom.readUInt32LE(i);
    const cmd = Buffer.from([0x15, word & 0xff, (word >> 8) & 0xff, (word >> 16) & 0xff, (word >> 24) & 0xff]);
    const r = await jb.xfer(cmd, 1);
    if (r[0] & JSTAT_VALID_MASK) throw new Error(`header WRITE bad jstat 0x${r[0].toString(16)} at i=${i}`);
  }

  // Step 6: stream encrypted body.
  console.log(`[mb] step 6: streaming body (${rom.length - 0xC0} bytes, encrypted)`);
  let fcrc = CRC_SEED;
  const t0 = Date.now();
  let i;
  for (i = 0xC0; i < rom.length; i += 4) {
    let plaintext = rom.readUInt32LE(i);
    if (i === 0xC4) plaintext = ((channel & 0xff) << 8) >>> 0;
    fcrc = crcStep(fcrc, plaintext);
    sessionKey = (Math.imul(sessionKey, MAGIC_KAWA) + 1) >>> 0;
    let enc = (plaintext ^ sessionKey) >>> 0;
    enc = (enc ^ ((~(i + (0x20 << 20)) + 1) >>> 0)) >>> 0;
    enc = (enc ^ MAGIC_BY) >>> 0;
    const cmd = Buffer.from([0x15, enc & 0xff, (enc >> 8) & 0xff, (enc >> 16) & 0xff, (enc >> 24) & 0xff]);
    const r = await jb.xfer(cmd, 1);
    if (r[0] & JSTAT_VALID_MASK) throw new Error(`body WRITE bad jstat 0x${r[0].toString(16)} at i=${i}`);
    if ((i & 0xFFF) === 0) {
      console.log(`[mb]   progress: 0x${i.toString(16)}/${rom.length.toString(16)} (${((i / rom.length) * 100).toFixed(1)}%)`);
    }
  }
  const dur = ((Date.now() - t0) / 1000).toFixed(2);
  console.log(`[mb]   body done in ${dur}s (${(rom.length - 0xC0)/dur/1024} KB/s)`);

  // Step 7: final fcrc word.
  console.log(`[mb] step 7: WRITE final fcrc=0x${(fcrc & 0xFFFF).toString(16)}`);
  {
    const finalWord = ((fcrc & 0xFFFF) | (rom.length << 16)) >>> 0;
    sessionKey = (Math.imul(sessionKey, MAGIC_KAWA) + 1) >>> 0;
    let enc = (finalWord ^ sessionKey) >>> 0;
    enc = (enc ^ ((~(i + (0x20 << 20)) + 1) >>> 0)) >>> 0;
    enc = (enc ^ MAGIC_BY) >>> 0;
    const cmd = Buffer.from([0x15, enc & 0xff, (enc >> 8) & 0xff, (enc >> 16) & 0xff, (enc >> 24) & 0xff]);
    const r = await jb.xfer(cmd, 1);
    if (r[0] & JSTAT_VALID_MASK) throw new Error(`fcrc WRITE bad jstat 0x${r[0].toString(16)}`);
  }

  // Step 8: READ crc_reply (BIOS's CRC check result). Generous timeout
  // because the firmware-side intercept-replay does its native upload
  // here, which takes ~1-2s for a 16KB ROM.
  console.log('[mb] step 8: READ crc_reply (waiting up to 10s)');
  {
    await jb.send(Buffer.from([0x14]));
    const r = await jb.recv(5, 10000);
    console.log(`[mb]   crc_reply: ${r.toString('hex')}`);
  }

  console.log('[mb] ✓ multiboot complete — GBA should now be running the uploaded ROM');
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

// ============================================================================
// CLI entry
// ============================================================================
const argv = process.argv.slice(2);
const romPath = argv[argv.indexOf('--rom') + 1] || '/tmp/joypad_mb.gba';
const rom = readFileSync(romPath);
console.log(`[main] loaded ROM ${romPath} (${rom.length} bytes)`);

const PORT = 54970;
let attempt = 0;
const server = createServer(async (sock) => {
  attempt += 1;
  const a = attempt;
  console.log(`\n[main] === attempt #${a} === CH9120 connected from ${sock.remoteAddress}:${sock.remotePort}`);
  sock.setNoDelay(true);
  const jb = new TcpJoybus(sock);
  try {
    await multiboot(jb, rom, /*channel=*/0);
    console.log(`[main] === attempt #${a} succeeded ===`);
  } catch (e) {
    console.error(`[main] === attempt #${a} failed: ${e.message} ===`);
  }
  // Force-close so the chip reconnects fresh; CH9120 TCP_CLIENT will retry.
  sock.end();
  sock.destroy();
});

server.listen(PORT, () => {
  console.log(`[main] listening on :${PORT} — reset gc2eth board to start.`);
});
