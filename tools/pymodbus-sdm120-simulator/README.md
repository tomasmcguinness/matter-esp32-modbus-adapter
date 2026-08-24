# pymodbus-sdm120-simulator

Simulates an Eastron SDM120M energy meter over Modbus RTU, so the adapter firmware
can be tested against a PC instead of (or alongside) the real meter or the
`tools/modbus-slave-simulator` hardware simulator.

Matches the registers `firmware/main/sdm120.cpp` reads (function code 0x04,
big-endian float32, no word swap):

| Register | Address | Value  |
|----------|---------|--------|
| Voltage  | 0x0000  | 230.5 V|
| Current  | 0x0006  | 1.23 A |
| Power    | 0x000C  | 283.2 W|
| Energy   | 0x001A  | 12.34 kWh|

Edit the `float32` values in `config.json` to test different readings.

## Hardware

PC → YP-05 (USB↔TTL) → HW-519 (TTL↔RS485, auto direction) → A/B → adapter's RS485 bus.

The HW-519 handles TX/RX direction switching itself, so no RTS/DE toggling is needed
on the PC side.

Only one Modbus RTU slave can be on the bus at a time — unplug/power down the
`modbus-slave-simulator` board (or the real SDM120M) before starting this simulator.

## Setup

```
pip install pymodbus[serial] aiohttp
```

(Already satisfied if you've installed `pymodbus`, `pyserial`, and `aiohttp` for
other tools in this repo.)

Find the YP-05's COM port in Device Manager (it enumerates as a CH340 USB Serial
device — distinct from the ESP32 boards, which show up as `USB\VID_303A...`).
Edit `"port"` in `config.json` to match, e.g. `"COM5"`.

## Usage

Run from this directory:

```
pymodbus.simulator --modbus_server sdm120 --modbus_device sdm120 --json_file config.json
```

(`pymodbus.simulator` is the console script `pip` installs alongside the package —
run `where pymodbus.simulator` if it's not on your PATH.)

This starts the RTU server on the configured COM port, plus a web console at
`http://localhost:8080` where you can watch requests/responses live and edit
register values without restarting. You'll see two deprecation warnings
(`ModbusSimulatorContext`/`ModbusServerContext` "will be removed in v4") on
startup — harmless on the current 3.x releases, just pymodbus flagging that the
config format changes in 4.0.

Then run the adapter firmware (or point `tools/uart-send-byte`/a Modbus master
at the same bus) and it should read back the values above.

## Notes

- `"type exception": true` means any register the firmware queries that isn't
  defined in `config.json` returns a Modbus exception, same as a real meter
  would for an unsupported register.
- Registers are read-only (`"write": []`), matching the real SDM120M.
