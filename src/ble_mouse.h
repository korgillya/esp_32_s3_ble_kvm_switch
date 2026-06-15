/**
 * @file ble_mouse.h
 * @brief BLE HID Mouse peripheral with dual-host instant switching.
 *
 * Owns the NimBLE peripheral role. Holds up to two simultaneous BLE
 * connections (one per slot / laptop), advertises per-slot identities, and
 * routes inbound mouse reports to the host that subscribed to the standard
 * HID Input Report. See [docs/architecture.md](../docs/architecture.md) for
 * the runtime state machine.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_events.h"
#include "esp_err.h"

/**
 * @brief Initialise NVS-aware NimBLE host, register HID + KVM Control GATT
 *        services, and start advertising.
 *
 * Called once from `ble_hid_task`. Reads `kvm_slots` to resume the last
 * active slot, prunes orphan bonds from the NimBLE store, then hands control
 * over to the NimBLE host task. Safe to call from a FreeRTOS task; not from
 * an ISR.
 *
 * @return ESP_OK on success, otherwise a propagated ESP-IDF error code.
 */
esp_err_t ble_mouse_start(void);

/**
 * @brief Enter pairing mode for the given slot.
 *
 * Clears that slot's NimBLE bond and `kvm_slots` mapping (the *other* slot
 * is untouched), terminates any active connection on that slot, then starts
 * connectable advertising under the slot's identity with no whitelist.
 * Sets `s_pending_pair_slot` so the next successful encryption is captured
 * as the new bond for this slot.
 *
 * @param host  Slot to (re-)pair.
 * @return ESP_OK once advertising has been (re-)started.
 */
esp_err_t ble_mouse_start_pairing(kvm_host_t host);

/**
 * @brief Switch which host the mouse reports are routed to.
 *
 * Updates `s_desired_slot` without disconnecting anything. If the target
 * slot is already connected and secure, subsequent `ble_mouse_send_report`
 * calls flip to that connection within a single GATT notification. If the
 * slot is disconnected with a stored bond, `update_advertising()` will try
 * to bring it back up.
 *
 * @param host  Target slot.
 * @retval ESP_OK            Slot has either a stored bond or is already up.
 * @retval ESP_ERR_NOT_FOUND  Slot has no bond yet; user must long-press to
 *                            pair before anything will happen.
 */
esp_err_t ble_mouse_select_host(kvm_host_t host);

/**
 * @brief Push a single 4-byte HID mouse report (buttons, dx, dy, wheel) to
 *        the currently desired slot.
 *
 * Routed via `s_hid_subscriber_conn[s_desired_slot]` so that mouse
 * notifications always reach the OS HID stack even if a companion app holds
 * a second BLE link on the same peer.
 *
 * @param buttons  Button bitmap (3 LSBs used).
 * @param dx       Relative X movement, -127..127.
 * @param dy       Relative Y movement, -127..127.
 * @param wheel    Relative wheel ticks, -127..127.
 * @retval ESP_OK              Notification queued for transmission.
 * @retval ESP_ERR_INVALID_STATE Desired slot has no HID-subscribing conn yet,
 *                              or the HID Input Report attribute handle has
 *                              not been resolved.
 * @retval ESP_ERR_NO_MEM       Could not allocate the notify mbuf.
 * @retval ESP_FAIL             NimBLE returned an error from notify_custom.
 */
esp_err_t ble_mouse_send_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

/** @return true if the desired slot has a HID subscriber and the link is encrypted. */
bool ble_mouse_is_connected(void);

/** @return true if any advertising session is currently active. */
bool ble_mouse_is_advertising(void);

/** @return true if the current advertising session was started in pairing mode. */
bool ble_mouse_is_pairing(void);

/**
 * @brief Look up which slot owns the given BLE connection.
 * @param conn_handle  Live NimBLE conn handle.
 * @param[out] out_slot Set to the resolved slot on success.
 * @return true if a slot owns this conn handle, false otherwise.
 */
bool ble_mouse_slot_for_conn(uint16_t conn_handle, kvm_host_t *out_slot);

/**
 * @brief Get the active encrypted conn handle for a slot.
 * @param slot  Slot index.
 * @return The conn handle, or `BLE_HS_CONN_HANDLE_NONE` (0xFFFF) if the slot
 *         is not yet encrypted.
 */
uint16_t ble_mouse_conn_for_slot(kvm_host_t slot);

/**
 * @brief Programmatically change the active slot, as if the user had pressed
 *        the corresponding button.
 *
 * Posts `KVM_EVENT_GATT_SWITCH_TO_*` into the state machine, so LEDs, buzzer,
 * OLED display, and NVS-persisted active slot all update through the same
 * code path as a manual switch. Used by `ble_control.c` when a companion
 * app reports an edge crossing.
 *
 * @param slot  New active slot.
 */
void ble_mouse_force_select(kvm_host_t slot);
