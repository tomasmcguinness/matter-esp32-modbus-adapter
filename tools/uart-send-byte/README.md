# uart-send-byte

Repeatedly sends a single byte over UART, once a second, using a YP-05 USB-to-serial adapter. Useful for quick hardware bring-up checks without flashing firmware. Stop with Ctrl+C.

## Setup

```
pip install pyserial
```

## Usage

### Sending

```
python send_byte.py --port COM5 --baud 9600 --byte 0x01
```

- `--port` (required) — serial port, e.g. `COM5` on Windows or `/dev/ttyUSB0` on Linux
- `--baud` (optional) — baud rate, default `9600`
- `--byte` (optional) — byte to send as decimal or hex (e.g. `0x01`), default `0x00`

### Receiving

```
python receive_byte.py --port COM5 --baud 9600
```

Listens on the given port and logs each byte received, with a timestamp, until stopped with Ctrl+C.

- `--port` (required) — serial port, e.g. `COM5` on Windows or `/dev/ttyUSB0` on Linux
- `--baud` (optional) — baud rate, default `9600`
