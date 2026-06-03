#!/usr/bin/env python3
"""Test if chip has per-packet vs per-byte latency."""
import socket, time, sys

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', 54970)); srv.listen(1)
print("[burst] waiting...")
sock, peer = srv.accept()
sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
print(f"[burst] connected {peer}")

# Test 1: 100 sequential 1-byte STATUSes
samples = []
for _ in range(100):
    t0 = time.perf_counter_ns()
    sock.sendall(b'\x00')
    got = b''
    while len(got) < 3:
        got += sock.recv(3-len(got))
    t1 = time.perf_counter_ns()
    samples.append((t1-t0)/1000)
samples.sort()
print(f"sequential 100x STATUS (1->3):  min={samples[0]:6.0f}us  p50={samples[len(samples)//2]:6.0f}us  total={sum(samples)/1000:.1f}ms")

# Test 2: 100 STATUSes pipelined — send all at once, then read all
t0 = time.perf_counter_ns()
sock.sendall(b'\x00' * 100)
got = b''
while len(got) < 300:
    got += sock.recv(300-len(got))
t1 = time.perf_counter_ns()
print(f"pipelined 100x STATUS in 1 burst:  total={(t1-t0)/1000000:.1f}ms  per-cmd={(t1-t0)/100/1000:.0f}us")

# Test 3: 50 WRITE-like (5 bytes -> 1 byte ack)
samples = []
for _ in range(50):
    t0 = time.perf_counter_ns()
    sock.sendall(b'\x15\x00\x00\x00\x00')
    got = b''
    while len(got) < 1:
        got += sock.recv(1-len(got))
    t1 = time.perf_counter_ns()
    samples.append((t1-t0)/1000)
samples.sort()
print(f"sequential 50x WRITE (5->1):  min={samples[0]:6.0f}us  p50={samples[len(samples)//2]:6.0f}us")

sock.close(); srv.close()
