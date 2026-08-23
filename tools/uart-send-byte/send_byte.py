#!/usr/bin/env python3
"""Repeatedly send a single byte over UART using a YP-05 USB-to-serial adapter."""

import argparse
import sys
import time
from datetime import datetime

import serial


def byte_value(value: str) -> int:
    parsed = int(value, 0)
    if not 0 <= parsed <= 255:
        raise argparse.ArgumentTypeError(f"{value} is not in range 0x00-0xFF")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=9600, help="Baud rate (default: 9600)")
    parser.add_argument(
        "--byte",
        type=byte_value,
        default=0x00,
        help="Byte to send, decimal or hex e.g. 0x01 (default: 0x00)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        with serial.Serial(args.port, args.baud, timeout=1) as ser:
            print(f"Sending 0x{args.byte:02X} on {args.port} @ {args.baud} baud, once a second. Press Ctrl+C to stop.")
            try:
                while True:
                    ser.write(bytes([args.byte]))
                    timestamp = datetime.now().strftime("%H:%M:%S")
                    print(f"[{timestamp}] Sent byte 0x{args.byte:02X}")
                    time.sleep(1)
            except KeyboardInterrupt:
                print("\nStopped.")
    except serial.SerialException as exc:
        print(f"Error: could not open {args.port}: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
