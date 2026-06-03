#!/usr/bin/env python3
"""
usbgba-burst-test.py — measure USB vendor-bulk RTT through the GBA-link bridge.

Sequel to eth-burst-test.py (TCP) — same idea, USB transport.
Compares apples-to-apples so we can tell whether the USB path
materialized the latency win we predicted (TCP ~2.4ms median →
USB target ~125-500µs).

Tests:
  1. 100 sequential STATUS commands (1 → 3 bytes)
  2. 50 sequential WRITE commands (5 → 1 bytes)
  3. Pipelined comparison if USB allows (queue all writes, then read)

Run with the GBA powered on at the BIOS multiboot listen screen for
realistic timing — without a GBA the bridge's joybus call adds a
5ms timeout penalty per command and skews the numbers.
"""

import argparse
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb not installed. Run: pip install pyusb", file=sys.stderr)
    sys.exit(1)


def find_and_claim(vid: int, pid: int):
    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        print(f"ERROR: no device at VID 0x{vid:04X} PID 0x{pid:04X}",
              file=sys.stderr)
        sys.exit(2)

    cfg = dev.get_active_configuration()
    iface = next((i for i in cfg if i.bInterfaceClass == 0xFF), None)
    if iface is None:
        print("ERROR: no vendor-class interface", file=sys.stderr)
        sys.exit(3)

    try:
        if dev.is_kernel_driver_active(iface.bInterfaceNumber):
            dev.detach_kernel_driver(iface.bInterfaceNumber)
    except (NotImplementedError, usb.core.USBError):
        pass

    usb.util.claim_interface(dev, iface.bInterfaceNumber)
    out_ep = next(ep for ep in iface
                  if usb.util.endpoint_direction(ep.bEndpointAddress)
                  == usb.util.ENDPOINT_OUT)
    in_ep = next(ep for ep in iface
                 if usb.util.endpoint_direction(ep.bEndpointAddress)
                 == usb.util.ENDPOINT_IN)
    return dev, iface, out_ep, in_ep


def percentile(samples_us, p):
    s = sorted(samples_us)
    k = max(0, min(len(s) - 1, int(len(s) * p / 100)))
    return s[k]


def bench(name, n_iters, tx_bytes, rx_len, out_ep, in_ep,
          settle_us=0):
    samples = []
    for _ in range(n_iters):
        if settle_us:
            time.sleep(settle_us / 1_000_000)
        t0 = time.perf_counter_ns()
        out_ep.write(tx_bytes, timeout=200)
        rx = in_ep.read(rx_len, timeout=200)
        t1 = time.perf_counter_ns()
        samples.append((t1 - t0) / 1000.0)
        assert len(rx) == rx_len, f"short read: {len(rx)} vs {rx_len}"
    s = sorted(samples)
    print(f"{name:50s}  "
          f"min={s[0]:6.0f}µs  p50={percentile(samples, 50):6.0f}µs  "
          f"p95={percentile(samples, 95):6.0f}µs  "
          f"max={s[-1]:6.0f}µs  total={sum(samples)/1000:.1f}ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=0x057E)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=0x0338)
    args = ap.parse_args()

    dev, iface, out_ep, in_ep = find_and_claim(args.vid, args.pid)
    print(f"[burst] connected to VID 0x{args.vid:04X} PID 0x{args.pid:04X}, "
          f"max-packet {out_ep.wMaxPacketSize}\n")

    bench("100x STATUS (1→3) sequential",
          100, b"\x00", 3, out_ep, in_ep)

    bench("50x WRITE (5→1) sequential",
          50, b"\x15\xAA\xBB\xCC\xDD", 1, out_ep, in_ep)

    bench("100x RESET (1→3) sequential",
          100, b"\xFF", 3, out_ep, in_ep)

    print()
    print("Compare to TCP path (eth-burst-test.py):")
    print("  100x STATUS sequential: TCP min=1344µs p50=2418µs")
    print("  50x WRITE sequential:   TCP min=2332µs p50=2455µs")
    print("If USB p50 < ~500µs we've achieved the architectural win.")

    usb.util.release_interface(dev, iface.bInterfaceNumber)


if __name__ == "__main__":
    main()
