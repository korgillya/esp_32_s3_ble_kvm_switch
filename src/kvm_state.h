/**
 * @file kvm_state.h
 * @brief Central KVM state machine + the struct that drives the OLED UI.
 *
 * `kvm_state_t` is the single source of truth for "what is the device doing
 * right now": which host is active, USB enumeration progress, BLE link
 * status per slot, last mouse report, and counters. `kvm_state_handle_event`
 * is the only function that mutates it, and it is only called from
 * `kvm_task`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_events.h"

/**
 * @brief Snapshot of every piece of state the firmware exposes to the UI.
 *
 * Updated only by `kvm_state_handle_event`. The UI / BLE / USB tasks read
 * fields directly; concurrent writes from elsewhere are not safe.
 */
typedef struct {
    kvm_host_t active_host;            ///< Slot the user / GATT picked.
    bool usb_host_started;             ///< USB host driver initialised.
    bool usb_device_seen;              ///< Any USB device was enumerated.
    bool usb_hid_seen;                 ///< A HID interface was found.
    bool usb_mouse_connected;          ///< A HID mouse is actively reporting.
    bool ble_connected[2];             ///< Per-slot BLE encrypted-link flag.
    uint16_t usb_vid;                  ///< Last USB VID seen.
    uint16_t usb_pid;                  ///< Last USB PID seen.
    uint8_t usb_proto;                 ///< HID protocol code (boot vs report).
    uint32_t mouse_report_count;       ///< Total reports forwarded to BLE.
    uint32_t last_mouse_report_ms;     ///< esp_timer ms of last report.
    int8_t last_mouse_dx;              ///< Last forwarded dx.
    int8_t last_mouse_dy;              ///< Last forwarded dy.
    int8_t last_mouse_wheel;           ///< Last forwarded wheel.
    uint8_t last_mouse_buttons;        ///< Last forwarded button bitmap.
    uint8_t last_mouse_raw_len;        ///< Raw USB report length for diagnostics.
    uint8_t last_mouse_raw[8];         ///< Raw USB report bytes for diagnostics.
    uint32_t switch_count;             ///< Cumulative manual + GATT switches.
    uint32_t last_switch_ms;           ///< esp_timer ms of last switch (debouncer).
    uint32_t dropped_events;           ///< Events that hit the debouncer or were ignored.
} kvm_state_t;

/**
 * @brief Initialise a `kvm_state_t` in-place to a clean post-boot state.
 *
 * Pulls the persisted active slot from `kvm_slots` and sets the LEDs to
 * match it, so a reset visually resumes where the user left off.
 */
void kvm_state_init(kvm_state_t *state);

/**
 * @brief Apply one queued event to the state machine.
 *
 * Mutates `*state` and may produce side effects on the board (LEDs, beep,
 * NVS persistence) or on BLE (`ble_mouse_select_host`, `ble_mouse_start_pairing`).
 *
 * @return true if the event was consumed / state changed, false if it was
 *         dropped (debouncer, no-op switch, unknown type).
 */
bool kvm_state_handle_event(kvm_state_t *state, const kvm_event_t *event);

/**
 * @brief Lower-case "left" / "right" string for logging.
 */
const char *kvm_host_name(kvm_host_t host);
