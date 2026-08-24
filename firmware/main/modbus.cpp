#include "modbus.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>

// Revision A Board. TX GPIO22, RX GPIO23, DE/RE GPIO18. 9600 baud, 8N1, no flow control.

#define MODBUS_UART     UART_NUM_1
#define TXD_PIN         ((gpio_num_t)22)
#define RXD_PIN         ((gpio_num_t)23)
#define DE_RE_PIN       ((gpio_num_t)18)
#define MODBUS_SLAVE_ADDR 1

#define RX_TEST_TIMEOUT_MS 2000
#define LOOPBACK_TIMEOUT_MS 200

// ESP-IDF's printf has no %b, so expand the byte into eight explicit bits.
#define BYTE_TO_BINARY_FMT "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(b)                               \
    ((b) & 0x80 ? '1' : '0'), ((b) & 0x40 ? '1' : '0'), \
    ((b) & 0x20 ? '1' : '0'), ((b) & 0x10 ? '1' : '0'), \
    ((b) & 0x08 ? '1' : '0'), ((b) & 0x04 ? '1' : '0'), \
    ((b) & 0x02 ? '1' : '0'), ((b) & 0x01 ? '1' : '0')

static const char *TAG = "Modbus";

static const uint16_t crc_table[] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};

static uint16_t crc16_modbus(uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++)
    {
        uint16_t tbl_idx = (crc ^ data[i]) & 0x0F;
        crc = (crc >> 4) ^ crc_table[tbl_idx];
        tbl_idx = (crc ^ (data[i] >> 4)) & 0x0F;
        crc = (crc >> 4) ^ crc_table[tbl_idx];
    }
    return crc;
}

static float bytes_to_float(const uint8_t *bytes)
{
    // Cast each byte before shifting: uint8_t promotes to int, so bytes[0] << 24
    // shifts into the sign bit for any value >= 0x80.
    uint32_t val = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
                   ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];

    // memcpy rather than a pointer cast; type-punning through *(float *)&val
    // breaks strict aliasing and the optimiser is free to reorder around it.
    float result;
    memcpy(&result, &val, sizeof(result));
    return result;
}

// Configures the UART and DE/RE pin. Pass a queue handle to receive driver
// events (framing errors, breaks); pass NULL for plain byte-oriented use.
static void modbus_uart_configure(QueueHandle_t *event_queue)
{
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    uart_param_config(MODBUS_UART, &uart_config);
    uart_set_pin(MODBUS_UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(MODBUS_UART, 256, 0, event_queue ? 20 : 0, event_queue, 0);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DE_RE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(DE_RE_PIN, 0); // Start in RX mode
}

void modbus_uart_init(void)
{
    modbus_uart_configure(NULL);
}

static void modbus_tx_test_task(void *arg)
{
    gpio_set_level(DE_RE_PIN, 1); // hold driver enabled for the whole test
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t byte = 0x48; // change freely - pick something easy to spot on a scope/logic analyzer
    while (1)
    {
        uart_write_bytes(MODBUS_UART, &byte, 1);
        uart_wait_tx_done(MODBUS_UART, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "TX 0x%02X (0b" BYTE_TO_BINARY_FMT ")", byte, BYTE_TO_BINARY(byte));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void modbus_start_tx_test(void)
{
    xTaskCreate(modbus_tx_test_task, "modbus_tx_test", 2048, NULL, 5, NULL);
}

static void modbus_rx_test_task(void *arg)
{
    // Reinstall the driver with an event queue. uart_read_bytes silently drops
    // framing errors, so a dead line and a garbled line look identical through
    // it; the event queue tells them apart.
    QueueHandle_t events = NULL;
    uart_driver_delete(MODBUS_UART);
    modbus_uart_configure(&events);

    gpio_set_level(DE_RE_PIN, 0); // hold the receiver enabled for the whole test
    vTaskDelay(pdMS_TO_TICKS(2));
    uart_flush_input(MODBUS_UART);

    ESP_LOGI(TAG, "RX test started, listening on UART%d", MODBUS_UART);

    while (1)
    {
        uart_event_t event;
        if (!xQueueReceive(events, &event, pdMS_TO_TICKS(RX_TEST_TIMEOUT_MS)))
        {
            ESP_LOGW(TAG, "RX idle (no bytes in %d ms)", RX_TEST_TIMEOUT_MS);
            continue;
        }

        switch (event.type)
        {
        case UART_DATA:
            for (size_t i = 0; i < event.size; i++)
            {
                uint8_t byte = 0;
                if (uart_read_bytes(MODBUS_UART, &byte, 1, 0) == 1)
                {
                    ESP_LOGI(TAG, "RX 0x%02X (0b" BYTE_TO_BINARY_FMT ")", byte, BYTE_TO_BINARY(byte));
                }
            }
            break;

        case UART_FRAME_ERR:
        {
            ESP_LOGE(TAG, "RX FRAMING ERROR - signal is arriving but the stop bit is wrong.");
            // Log whatever the driver kept, if anything. If the data bits are
            // correct and only the stop bit failed, the sender is releasing the
            // bus before the frame completes (auto-direction turnaround).
            uint8_t byte = 0;
            while (uart_read_bytes(MODBUS_UART, &byte, 1, 0) == 1)
            {
                ESP_LOGE(TAG, "  bad frame carried 0x%02X (0b" BYTE_TO_BINARY_FMT ")",
                         byte, BYTE_TO_BINARY(byte));
            }
            break;
        }

        case UART_BREAK:
            ESP_LOGE(TAG, "RX BREAK - line held low for a full frame.");
            ESP_LOGE(TAG, "  Nobody is driving the bus. A terminated pair with no failsafe");
            ESP_LOGE(TAG, "  bias sits at 0V differential, which the MAX485 cannot resolve.");
            break;

        case UART_PARITY_ERR:
            ESP_LOGE(TAG, "RX PARITY ERROR - sender is not 8N1.");
            break;

        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "RX overflow - data arriving faster than it is drained.");
            uart_flush_input(MODBUS_UART);
            break;

        default:
            ESP_LOGW(TAG, "RX event type %d", (int)event.type);
            break;
        }
    }
}

void modbus_start_rx_test(void)
{
    xTaskCreate(modbus_rx_test_task, "modbus_rx_test", 2048, NULL, 5, NULL);
}

static void modbus_loopback_test_task(void *arg)
{
    ESP_LOGI(TAG, "Loopback test started. Jumper GPIO%d -> GPIO%d to bypass the",
             (int)TXD_PIN, (int)RXD_PIN);
    ESP_LOGI(TAG, "transceiver entirely; leave it wired to echo through the bus.");

    uint8_t tx = 0x48;
    while (1)
    {
        uart_flush_input(MODBUS_UART); // discard noise before we listen for our own byte

        gpio_set_level(DE_RE_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        uart_write_bytes(MODBUS_UART, &tx, 1);
        uart_wait_tx_done(MODBUS_UART, pdMS_TO_TICKS(100));
        gpio_set_level(DE_RE_PIN, 0); // back to receive as soon as the stop bit is out

        uint8_t rx = 0;
        int len = uart_read_bytes(MODBUS_UART, &rx, 1, pdMS_TO_TICKS(LOOPBACK_TIMEOUT_MS));

        if (len == 1 && rx == tx)
        {
            ESP_LOGI(TAG, "LOOPBACK OK  sent 0x%02X, got 0x%02X", tx, rx);
        }
        else if (len == 1)
        {
            ESP_LOGW(TAG, "LOOPBACK CORRUPT  sent 0x%02X (0b" BYTE_TO_BINARY_FMT "), "
                          "got 0x%02X (0b" BYTE_TO_BINARY_FMT ")",
                     tx, BYTE_TO_BINARY(tx), rx, BYTE_TO_BINARY(rx));
        }
        else
        {
            ESP_LOGE(TAG, "LOOPBACK FAIL  sent 0x%02X, nothing came back in %d ms",
                     tx, LOOPBACK_TIMEOUT_MS);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void modbus_start_loopback_test(void)
{
    xTaskCreate(modbus_loopback_test_task, "modbus_loopback", 2048, NULL, 5, NULL);
}

esp_err_t modbus_read_float_register(uint16_t reg_addr, float *result)
{
    uart_flush_input(MODBUS_UART);
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t request[8];
    request[0] = MODBUS_SLAVE_ADDR;
    request[1] = 0x04;
    request[2] = (reg_addr >> 8) & 0xFF;
    request[3] = reg_addr & 0xFF;
    request[4] = 0x00;
    request[5] = 0x02;

    uint16_t crc = crc16_modbus(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    ESP_LOGI(TAG, "Requesting reg 0x%04X", reg_addr);
    ESP_LOG_BUFFER_HEX(TAG, request, sizeof(request));

    ESP_LOGD(TAG, "DE/RE -> TX");
    gpio_set_level(DE_RE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    uart_write_bytes(MODBUS_UART, request, 8);
    uart_wait_tx_done(MODBUS_UART, pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGD(TAG, "DE/RE -> RX");
    gpio_set_level(DE_RE_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t response[9];
    int len = uart_read_bytes(MODBUS_UART, response, 9, pdMS_TO_TICKS(1000));

    if (len > 0)
    {
        ESP_LOGI(TAG, "Received %d bytes", len);
        ESP_LOG_BUFFER_HEX(TAG, response, len);
    }

    if (len != 9)
    {
        ESP_LOGE(TAG, "No response (got %d bytes)", len);
        return ESP_FAIL;
    }

    uint16_t response_crc = crc16_modbus(response, 7);
    uint16_t received_crc = response[7] | (response[8] << 8);

    if (response_crc != received_crc)
    {
        ESP_LOGE(TAG, "CRC mismatch");
        return ESP_FAIL;
    }

    *result = bytes_to_float(&response[3]);
    ESP_LOGI(TAG, "reg 0x%04X = %f", reg_addr, *result);
    return ESP_OK;
}
