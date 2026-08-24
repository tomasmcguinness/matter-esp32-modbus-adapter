#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define MODBUS_UART   UART_NUM_1
#define TXD_PIN       ((gpio_num_t)18)  // D9 on XIAO ESP32-C6
#define RXD_PIN       ((gpio_num_t)20)  // D10 on XIAO ESP32-C6
#define DE_RE_PIN     ((gpio_num_t)2)   // D2 on XIAO ESP32-C6 — tie MAX485 DE and RE together
#define SLAVE_ADDR    1

static const char *TAG = "modbus-slave";

// Simulated SDM120M input registers (each float = 2 x uint16_t, big-endian word order)
// Edit these values to test different scenarios
static const float VOLTAGE      = 230.5f;
static const float CURRENT      = 1.23f;
static const float ACTIVE_POWER = 283.2f;
static const float TOTAL_ENERGY = 12.34f;

static const uint16_t crc_table[] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
};

static uint16_t crc16(uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        uint16_t idx = (crc ^ data[i]) & 0x0F;
        crc = (crc >> 4) ^ crc_table[idx];
        idx = (crc ^ (data[i] >> 4)) & 0x0F;
        crc = (crc >> 4) ^ crc_table[idx];
    }
    return crc;
}

static void float_to_regs(float value, uint16_t *hi, uint16_t *lo)
{
    uint32_t raw;
    memcpy(&raw, &value, sizeof(float));
    *hi = (raw >> 16) & 0xFFFF;
    *lo =  raw        & 0xFFFF;
}

// Returns false if the address is not a known register pair
static bool lookup_register(uint16_t addr, uint16_t *hi, uint16_t *lo)
{
    switch (addr) {
        case 0x0000: float_to_regs(VOLTAGE,      hi, lo); return true;
        case 0x0006: float_to_regs(CURRENT,      hi, lo); return true;
        case 0x000C: float_to_regs(ACTIVE_POWER, hi, lo); return true;
        case 0x001A: float_to_regs(TOTAL_ENERGY, hi, lo); return true;
        default: return false;
    }
}

static void modbus_slave_task(void *arg)
{
    uint8_t req[8];
    while (1) {
        int len = uart_read_bytes(MODBUS_UART, req, sizeof(req), pdMS_TO_TICKS(1000));
        if (len <= 0)
            continue;

        ESP_LOG_BUFFER_HEX(TAG, req, len);

        if (len != 8) {
            ESP_LOGW(TAG, "Unexpected frame length %d", len);
            continue;
        }

        if (req[0] != SLAVE_ADDR || req[1] != 0x04) {
            ESP_LOGW(TAG, "Ignoring frame addr=%d fc=0x%02X", req[0], req[1]);
            continue;
        }

        uint16_t req_crc = crc16(req, 6);
        uint16_t got_crc = req[6] | (req[7] << 8);
        if (req_crc != got_crc) {
            ESP_LOGW(TAG, "CRC mismatch in request");
            continue;
        }

        uint16_t reg_addr = (req[2] << 8) | req[3];
        uint16_t count    = (req[4] << 8) | req[5];

        if (count != 2) {
            ESP_LOGW(TAG, "Unexpected register count %d", count);
            continue;
        }

        uint16_t hi, lo;
        if (!lookup_register(reg_addr, &hi, &lo)) {
            ESP_LOGW(TAG, "Unknown register 0x%04X", reg_addr);
            continue;
        }

        // Build FC04 response: [addr][fc][byte_count][hi_hi][hi_lo][lo_hi][lo_lo][crc_lo][crc_hi]
        uint8_t resp[9];
        resp[0] = SLAVE_ADDR;
        resp[1] = 0x04;
        resp[2] = 4;           // 2 registers × 2 bytes
        resp[3] = (hi >> 8) & 0xFF;
        resp[4] =  hi        & 0xFF;
        resp[5] = (lo >> 8) & 0xFF;
        resp[6] =  lo        & 0xFF;
        uint16_t resp_crc = crc16(resp, 7);
        resp[7] = resp_crc & 0xFF;
        resp[8] = (resp_crc >> 8) & 0xFF;

        ESP_LOGI(TAG, "FC04 reg=0x%04X -> responding", reg_addr);

        gpio_set_level(DE_RE_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        uart_write_bytes(MODBUS_UART, resp, sizeof(resp));
        uart_wait_tx_done(MODBUS_UART, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(DE_RE_PIN, 0);
    }
}

static void heartbeat_task(void *arg)
{
    while (1) {
        ESP_LOGI(TAG, "alive");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = 9600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(MODBUS_UART, &uart_cfg);
    uart_set_pin(MODBUS_UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(MODBUS_UART, 256, 0, 0, NULL, 0);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DE_RE_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(DE_RE_PIN, 0);  // start in RX mode

    ESP_LOGI(TAG, "Modbus slave simulator ready (addr=%d, 9600 8N1)", SLAVE_ADDR);
    xTaskCreate(modbus_slave_task, "modbus_slave", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL);
}
