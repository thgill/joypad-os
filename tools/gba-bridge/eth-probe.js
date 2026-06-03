#!/usr/bin/env node
// eth-probe.js — minimal "fake Dolphin" that accepts the CH9120's
// outbound TCP, sends 0x00 STATUS once per second, prints the reply.
//
// Use to confirm the gc2eth firmware is wiring TCP bytes through to
// real joybus on GP2 and a real GBA is responding. With a GBA in
// multiboot wait state you should see:
//   rx=000410  (type 0x0400, jstat PSF0 set)
// With no GBA on the wire:
//   rx=000000

import { createServer } from 'node:net';

const PORT = 54970;
const POLL_MS = 1000;

const server = createServer((sock) => {
  const peer = `${sock.remoteAddress}:${sock.remotePort}`;
  console.log(`[probe] CH9120 connected from ${peer}`);

  let pending = Buffer.alloc(0);
  sock.on('data', (chunk) => {
    pending = Buffer.concat([pending, chunk]);
    while (pending.length >= 3) {
      const r = pending.subarray(0, 3);
      pending = pending.subarray(3);
      const type = r[0] | (r[1] << 8);
      const jstat = r[2];
      const psf0 = (jstat & 0x10) ? ' PSF0' : '';
      const recv = (jstat & 0x02) ? ' RECV' : '';
      const send = (jstat & 0x08) ? ' SEND' : '';
      const gba = type === 0x0400 ? ' (GBA!)' : '';
      console.log(`[probe] rx ${r.toString('hex')}  type=0x${type.toString(16).padStart(4,'0')}${gba} jstat=0x${jstat.toString(16).padStart(2,'0')}${psf0}${recv}${send}`);
    }
  });

  const tick = setInterval(() => {
    if (sock.destroyed) { clearInterval(tick); return; }
    sock.write(Buffer.from([0x00]));   // STATUS
  }, POLL_MS);

  sock.on('close', () => {
    clearInterval(tick);
    console.log(`[probe] CH9120 disconnected`);
  });

  sock.on('error', (e) => console.log(`[probe] socket error: ${e.message}`));
});

server.listen(PORT, () => {
  console.log(`[probe] listening on :${PORT} — waiting for gc2eth to connect…`);
});
