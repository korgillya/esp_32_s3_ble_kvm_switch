/**
 * @file ble_control.h
 * @brief Custom KVM Control GATT service used by the companion app.
 *
 * Exposes three characteristics that let a user-space app on each host
 * report cursor edge crossings and receive warp commands. See
 * [docs/gatt-contract.md](../docs/gatt-contract.md) for the binary
 * payload layout and UUIDs, and [docs/architecture.md](../docs/architecture.md)
 * for the auto-switching sequence.
 */

#pragma once

#include "esp_err.h"
#include "host/ble_uuid.h"

/**
 * @brief Service UUID exposed by `ble_control_register`.
 *
 * Companion apps filter their BLE scan by this UUID; on macOS this is what
 * makes already-bonded peripherals discoverable through
 * `scanForPeripheralsWithServices`.
 */
extern const ble_uuid128_t BLE_KVM_CONTROL_SVC_UUID;

/**
 * @brief Register the KVM Control GATT service with NimBLE.
 *
 * Must be called **after** `ble_store_config_init()` and **before**
 * `esp_nimble_enable()` so that the service definition is included in the
 * final GATT DB. Sets up `slot_id` (read), `edge_event` (write), and
 * `warp_cmd` (notify) characteristics. Their access callbacks live in
 * `ble_control.c` and call into `ble_mouse_force_select` /
 * `ble_gatts_notify_custom` to actually perform the switch and notify the
 * other host.
 *
 * @retval ESP_OK   Service queued for registration.
 * @retval ESP_FAIL `ble_gatts_count_cfg` or `ble_gatts_add_svcs` failed.
 */
esp_err_t ble_control_register(void);
