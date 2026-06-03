#!/usr/bin/env python3
"""UDP ping test — measure CH9120 UDP roundtrip latency.

CH9120 is on en10 (USB-Ethernet adapter). Mac may have multiple interfaces
on 192.168.1/24; we must force traffic out the right one or routes leak.
"""
import socket, time, struct, sys

PORT = 54970
CHIP_IP = '192.168.1.250'
LOCAL_IP = '192.168.1.159'  # en10's IP

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

# CRITICAL: Mac has the same IP on both en0 (Wi-Fi) and en10 (USB-ethernet).
# Default route is en0 — bind alone doesn't redirect outbound traffic.
# Use IP_BOUND_IF (macOS-specific, value 25) to force the socket to en10.
IP_BOUND_IF = 25
en10_idx = socket.if_nametoindex('en10')
print(f"[udp-ping] forcing socket to en10 (idx={en10_idx})")
sock.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF, en10_idx)

sock.bind(('0.0.0.0', PORT))
sock.settimeout(2.0)
chip_addr = (CHIP_IP, PORT)
print(f"[udp-ping] sending to {CHIP_IP}:{PORT} (port {PORT} via en10)")

# Ping test — sequential STATUS commands
samples = []
for i in range(50):
    t0 = time.perf_counter_ns()
    sock.sendto(b'\x00', chip_addr)
    try:
        data, _ = sock.recvfrom(64)
        t1 = time.perf_counter_ns()
        samples.append((t1-t0)/1000)
    except socket.timeout:
        print(f"[udp-ping] sample {i}: timeout")
        continue
if samples:
    samples.sort()
    n = len(samples)
    print(f"\n=== {len(samples)} UDP roundtrips ===")
    print(f"min : {samples[0]:8.1f} µs")
    print(f"p50 : {samples[n//2]:8.1f} µs")
    print(f"p90 : {samples[int(n*0.9)]:8.1f} µs")
    print(f"max : {samples[-1]:8.1f} µs")
    print(f"avg : {sum(samples)/n:8.1f} µs")
else:
    print("[udp-ping] no successful samples")

sock.close()
