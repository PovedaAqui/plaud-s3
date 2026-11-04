#!/usr/bin/env python3
"""Set the recorder's clock over USB.

    python3 scripts/sync_usb_time.py [--port COM4]

Sends `TIME <epoch> <utc_offset_seconds>` on the USB Serial/JTAG port. Useful
when the device has no Wi-Fi: the watch face shows local time immediately and
keeps running from the internal clock.
"""

from __future__ import annotations

import argparse
import datetime
import time

import serial


def default_port() -> str:
    import sys

    return "COM4" if sys.platform.startswith("win") else "/dev/ttyACM0"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port())
    args = parser.parse_args()

    now = datetime.datetime.now().astimezone()
    offset = int(now.utcoffset().total_seconds())
    command = f"TIME {int(time.time())} {offset}\n"
    with serial.Serial(args.port, 115200, timeout=1) as port:
        # Opening the port resets the ESP32; give it time to boot and start listening.
        port.reset_input_buffer()
        time.sleep(3.0)
        port.reset_input_buffer()
        port.write(command.encode())
        port.flush()
        deadline = time.time() + 3.0
        confirmed = False
        while time.time() < deadline:
            line = port.readline().decode(errors="replace").strip()
            if "TIME OK" in line or "clock set over USB" in line:
                confirmed = True
                break
            if line:
                print("device:", line)
    print(f"sent {command.strip()} to {args.port} ({now:%H:%M %Z})"
          + (" — confirmed" if confirmed else " — no acknowledgement"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
