#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_events.h"
#include "esp_err.h"

esp_err_t ble_mouse_start(void);
esp_err_t ble_mouse_start_pairing(kvm_host_t host);
esp_err_t ble_mouse_select_host(kvm_host_t host);
esp_err_t ble_mouse_send_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
bool ble_mouse_is_connected(void);
bool ble_mouse_is_advertising(void);
bool ble_mouse_is_pairing(void);

/* Resolve which slot owns the given conn_handle. Returns true on hit. */
bool ble_mouse_slot_for_conn(uint16_t conn_handle, kvm_host_t *out_slot);
/* Current secure-connected conn_handle for slot, or 0xFFFF if not available. */
uint16_t ble_mouse_conn_for_slot(kvm_host_t slot);
/* Force-change the active slot from inside another module (e.g. control service). */
void ble_mouse_force_select(kvm_host_t slot);
