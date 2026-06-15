/**
 * @file board_io.h
 * @brief Discrete I/O: buttons, host-indicator LEDs, buzzer, I2C for OLED.
 *
 * Pinout is defined in `app_config.h`. Buttons are read **active HIGH**
 * with internal pull-down; they are debounced by `button_scan_task` (3
 * consecutive 20 ms samples) and produce `KVM_EVENT_BUTTON_*` (short press,
 * fires on release) or `KVM_EVENT_PAIR_*` (long press ≥1.5 s, fires once).
 */

#pragma once

#include <stdbool.h>

#include "app_events.h"
#include "esp_err.h"

/**
 * @brief Configure GPIOs for LEDs / buttons, the LEDC channel for the buzzer,
 *        and start the polled button-scan FreeRTOS task.
 */
esp_err_t board_io_init(void);

/**
 * @brief Initialise the I2C bus used by the SSD1306 OLED.
 *
 * Idempotent: a second call returns ESP_OK if the driver is already loaded.
 */
esp_err_t board_i2c_init(void);

/**
 * @brief Light the LED of the active host and turn the other off.
 */
void board_set_active_host_leds(kvm_host_t host);

/**
 * @brief Log a USB connect/disconnect transition (no GPIO side effect).
 */
void board_set_usb_status(bool connected);

/**
 * @brief Short buzzer chirp on a host-switch action.
 *
 * Two different tones distinguish LEFT vs RIGHT so the user can tell which
 * way they just switched without looking at the OLED.
 */
void board_beep_switch(kvm_host_t host);

/**
 * @brief Two short high beeps signalling that a long-press has crossed the
 *        pairing threshold and pairing mode has been entered.
 */
void board_beep_pair(void);
