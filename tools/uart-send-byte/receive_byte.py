#!/usr/bin/env python3
"""Receive and log bytes over UART using a YP-05 USB-to-serial adapter."""

import argparse
import sys
from datetime import datetime

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=9600, help="Baud rate (default: 9600)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        with serial.Serial(args.port, args.baud, timeout=1) as ser:
            print(f"Listening on {args.port} @ {args.baud} baud. Press Ctrl+C to stop.")
            try:
                while True:
                    data = ser.read(1)
                    if data:
                        timestamp = datetime.now().strftime("%H:%M:%S")
                        print(f"[{timestamp}] Received byte 0x{data[0]:02X}")
            except KeyboardInterrupt:
                print("\nStopped.")
    except serial.SerialException as exc:
        print(f"Error: could not open {args.port}: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
