#pragma once

#include "esp_err.h"
#include "host/ble_uuid.h"

/* Service UUID exposed by ble_control. Companion apps filter their BLE scan
 * by this UUID; on macOS this is what makes already-bonded peripherals
 * discoverable through scanForPeripheralsWithServices. */
extern const ble_uuid128_t BLE_KVM_CONTROL_SVC_UUID;

/* Register the KVM Control GATT service with NimBLE.
 * Must be called after ble_store_config_init() and before esp_nimble_enable(). */
esp_err_t ble_control_register(void);
