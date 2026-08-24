#pragma once

#include "esp_err.h"
#include <stdint.h>

void modbus_uart_init(void);
esp_err_t modbus_read_float_register(uint16_t reg_addr, float *result);

// Bring-up helpers for verifying the physical RS-485 link independent of
// Modbus framing. Both start a task that never returns.

// Holds the driver enabled and repeatedly transmits a single byte.
void modbus_start_tx_test(void);

// Holds the receiver enabled and logs every byte that arrives, one at a time.
void modbus_start_rx_test(void);

// Transmits a byte and immediately listens for it. With TXD jumpered straight
// to RXD this isolates the ESP32's own UART RX path from the transceiver and
// the bus; left wired normally it is an echo test of the whole loop.
void modbus_start_loopback_test(void);
