#include "sdm120.h"
#include "modbus.h"
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

extern "C" void matter_update_voltage(float voltage_v);
extern "C" void matter_update_current(float current_a);
extern "C" void matter_update_power(float power_w);
#if CONFIG_ENABLE_STATUS_DISPLAY
extern "C" void status_display_update(float voltage, float current, float power, float energy);
#endif

static const char *TAG = "SDM120M";

void sdm120_read_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for serial to settle

    while (1)
    {
        float voltage = 0, current = 0, power = 0, energy = 0;

        ESP_LOGI(TAG, "Reading SDM120M...");

        status_led_flash();

        if (modbus_read_float_register(0, &voltage) == ESP_OK)
        {
            ESP_LOGI(TAG, "Voltage: %.2f V", voltage);
            matter_update_voltage(voltage);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to read voltage");
        }

        if (modbus_read_float_register(6, &current) == ESP_OK)
        {
            ESP_LOGI(TAG, "Current: %.2f A", current);
            matter_update_current(current);
        }

        if (modbus_read_float_register(12, &power) == ESP_OK)
        {
            ESP_LOGI(TAG, "Active Power: %.2f W", power);
            matter_update_power(power);
        }

        if (modbus_read_float_register(26, &energy) == ESP_OK)
        {
            ESP_LOGI(TAG, "Total Energy: %.2f kWh", energy);
        }

#if CONFIG_ENABLE_STATUS_DISPLAY
        status_display_update(voltage, current, power, energy);
#endif

        ESP_LOGI(TAG, "---");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
