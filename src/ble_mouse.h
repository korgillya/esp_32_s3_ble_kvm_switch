#pragma once

#include <stdbool.h>

#include "app_events.h"
#include "esp_err.h"

esp_err_t ble_mouse_start(void);
esp_err_t ble_mouse_start_pairing(kvm_host_t host);
esp_err_t ble_mouse_select_host(kvm_host_t host);
esp_err_t ble_mouse_send_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
bool ble_mouse_is_connected(void);
bool ble_mouse_is_advertising(void);
bool ble_mouse_is_pairing(void);
