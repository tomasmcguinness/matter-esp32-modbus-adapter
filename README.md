# Matter ESP32 Modbus Adapter
The goal of this project is to provide a simple working Modbus adapter. It is designed for the ESP32-C6 MCU, so can be used with ESPHome, ESP-IDF and Arduino.

> [!WARNING]
> This is a work in progress. The code is working without issue, but the PCB (Revision A) has some design issues, which need addressing.

# Hardware

The hardware folder contains a KiCad PCB design, designed around the ESP32-C6-MINI-1. Both the antenna and non-antenna versions will work here.

# SDM120M - Electrical Sensor

The first device supported by this project is the Eastron SDM120M Single Phase Energy Meter

The readings from this device will be exposed using as a Matter Electrical Sensor Device Type.