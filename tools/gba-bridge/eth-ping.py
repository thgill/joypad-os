#!/usr/bin/env python3
"""eth-ping.py — measure raw single-byte TCP roundtrip latency to gc2eth.

Tight blocking socket loop, no async / event-loop overhead. Sends one
byte, waits for one byte response, times the roundtrip. Repeats N times.

Use to check whether the ~10ms/WRITE measured via Node.js's eth-multiboot
is the CH9120 chip's real latency floor (in which case Madden multiboot
is fundamentally too slow) or just Node.js's await scheduling overhead
(in which case real Dolphin's C++ TCP would be faster and Madden should
actually work).

Sends RESET (0xFF) bytes — firmware passthrough forwards to GBA which
must be in BIOS multiboot wait state. Each RESET gets a 3-byte response
back so this also exercises the multi-byte response path.
"""
import socket, time, sys

PORT = 54970
N = int(sys.argv[1]) if len(sys.argv) > 1 else 100

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', PORT))
srv.listen(1)
print(f"[ping] listening :{PORT} — reset gc2eth to connect")
sock, peer = srv.accept()
sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
print(f"[ping] connected from {peer}")

# Use STATUS (0x00) — no side effects on GBA, 3-byte response.
cmd = b'\x00'

# Warm up
for _ in range(3):
    sock.sendall(cmd)
    sock.recv(3)

samples = []
for i in range(N):
    t0 = time.perf_counter_ns()
    sock.sendall(cmd)
    got = b''
    while len(got) < 3:
        got += sock.recv(3 - len(got))
    t1 = time.perf_counter_ns()
    samples.append((t1 - t0) / 1000.0)  # microseconds

samples.sort()
n = len(samples)
print(f"\n=== {N} STATUS roundtrips ===")
print(f"min : {samples[0]:8.1f} µs")
print(f"p50 : {samples[n//2]:8.1f} µs")
print(f"p90 : {samples[int(n*0.9)]:8.1f} µs")
print(f"p99 : {samples[int(n*0.99)]:8.1f} µs")
print(f"max : {samples[-1]:8.1f} µs")
print(f"avg : {sum(samples)/n:8.1f} µs")

# If avg is <2ms, the chip is fast and our 44s body upload via Node was
# Node's overhead. Real Dolphin in C++ should multiboot Madden fine.
# If avg is >5ms, the chip is the bottleneck.

sock.close()
srv.close()
