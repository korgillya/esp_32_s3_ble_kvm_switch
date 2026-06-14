#pragma once

#include <stdbool.h>

#include "app_events.h"
#include "esp_err.h"

esp_err_t board_io_init(void);
esp_err_t board_i2c_init(void);

void board_set_active_host_leds(kvm_host_t host);
void board_set_usb_status(bool connected);
void board_beep_switch(kvm_host_t host);
void board_beep_pair(void);
