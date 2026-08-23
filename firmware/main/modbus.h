#pragma once

#include "esp_err.h"
#include <stdint.h>

void modbus_uart_init(void);
esp_err_t modbus_read_float_register(uint16_t reg_addr, float *result);

// Bring-up helper: holds the driver enabled and repeatedly transmits a
// single byte, for verifying the physical RS-485 link independent of
// Modbus framing. Never returns.
void modbus_start_tx_test(void);
