#include "status_led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Revision A board. Status LED D2 on GPIO14, driven through R8 to GND, so a
// high level lights it.

#define STATUS_LED_PIN ((gpio_num_t)14)

#define SLOW_BLINK_MS 1000
#define FAST_BLINK_MS 100
#define FLASH_MS      50

static const char *TAG = "status_led";

// Written from the Matter event thread, read by the LED task.
static volatile status_led_state_t sState = STATUS_LED_OPERATIONAL;
static TaskHandle_t sTaskHandle = NULL;

static void status_led_task(void *arg)
{
    bool on = false;

    while (1)
    {
        // Latch the state for this iteration; it can change under us at any point.
        status_led_state_t state = sState;

        switch (state)
        {
        case STATUS_LED_READY_TO_PAIR:
        case STATUS_LED_PAIRING:
            on = !on;
            gpio_set_level(STATUS_LED_PIN, on);
            // The notification doubles as the half-period delay, so a state
            // change takes effect at once instead of after the current half-period.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(state == STATUS_LED_PAIRING ? FAST_BLINK_MS : SLOW_BLINK_MS));
            break;

        case STATUS_LED_OPERATIONAL:
            gpio_set_level(STATUS_LED_PIN, 0);
            on = false;
            // A notification here is either a flash request or a state change.
            // Re-reading the state tells the two apart.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (sState == STATUS_LED_OPERATIONAL)
            {
                gpio_set_level(STATUS_LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(FLASH_MS));
                gpio_set_level(STATUS_LED_PIN, 0);
            }
            break;

        case STATUS_LED_OFF:
        default:
            gpio_set_level(STATUS_LED_PIN, 0);
            on = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            break;
        }
    }
}

void status_led_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << STATUS_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(STATUS_LED_PIN, 0);

    // Start operational (dark). An uncommissioned board gets
    // kCommissioningWindowOpened moments after Matter starts and drops into the
    // slow blink, so no boot-time fabric lookup is needed here.
    sState = STATUS_LED_OPERATIONAL;

    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 5, &sTaskHandle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create the status LED task");
    }
}

void status_led_set_state(status_led_state_t state)
{
    sState = state;

    // Wake the task so it picks up the new state immediately.
    if (sTaskHandle != NULL)
    {
        xTaskNotifyGive(sTaskHandle);
    }
}

void status_led_flash(void)
{
    // Non-blocking, so the caller's timing is unaffected. In a blinking state
    // the task consumes this as an early wake and carries on with its pattern.
    if (sTaskHandle != NULL)
    {
        xTaskNotifyGive(sTaskHandle);
    }
}
