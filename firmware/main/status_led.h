#pragma once

enum status_led_state_t
{
    STATUS_LED_OFF,
    STATUS_LED_READY_TO_PAIR, // slow blink
    STATUS_LED_PAIRING,       // rapid blink
    STATUS_LED_OPERATIONAL,   // dark, flashes on Modbus activity
};

void status_led_init(void);
void status_led_set_state(status_led_state_t state);

// One-shot flash. Only visible in STATUS_LED_OPERATIONAL; ignored in the
// blinking states, where the pattern already occupies the LED.
void status_led_flash(void);
