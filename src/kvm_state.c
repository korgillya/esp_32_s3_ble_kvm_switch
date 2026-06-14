#include "kvm_state.h"

#include <string.h>

#include "app_config.h"
#include "ble_mouse.h"
#include "board_io.h"
#include "esp_log.h"
#include "kvm_slots.h"

static const char *TAG = "kvm-state";

void kvm_state_init(kvm_state_t *state)
{
    kvm_host_t persisted = KVM_HOST_LEFT;
    kvm_slots_get_active(&persisted);
    *state = (kvm_state_t){
        .active_host = persisted,
        .usb_host_started = false,
        .usb_device_seen = false,
        .usb_hid_seen = false,
        .usb_mouse_connected = false,
        .ble_connected = {false, false},
        .usb_vid = 0,
        .usb_pid = 0,
        .usb_proto = 0,
        .mouse_report_count = 0,
        .last_mouse_report_ms = 0,
        .last_mouse_dx = 0,
        .last_mouse_dy = 0,
        .last_mouse_wheel = 0,
        .last_mouse_buttons = 0,
        .last_mouse_raw_len = 0,
        .last_mouse_raw = {0},
        .switch_count = 0,
        .last_switch_ms = 0,
        .dropped_events = 0,
    };
    board_set_active_host_leds(persisted);
}

const char *kvm_host_name(kvm_host_t host)
{
    return host == KVM_HOST_LEFT ? "left" : "right";
}

static bool is_button_event(kvm_event_type_t type)
{
    return type == KVM_EVENT_BUTTON_LEFT || type == KVM_EVENT_BUTTON_RIGHT ||
           type == KVM_EVENT_GATT_SWITCH_TO_LEFT || type == KVM_EVENT_GATT_SWITCH_TO_RIGHT;
}

static kvm_host_t event_target_host(const kvm_event_t *event)
{
    switch (event->type) {
    case KVM_EVENT_BUTTON_LEFT:
    case KVM_EVENT_GATT_SWITCH_TO_LEFT:
        return KVM_HOST_LEFT;
    case KVM_EVENT_BUTTON_RIGHT:
    case KVM_EVENT_GATT_SWITCH_TO_RIGHT:
        return KVM_HOST_RIGHT;
    default:
        return event->host;
    }
}

bool kvm_state_handle_event(kvm_state_t *state, const kvm_event_t *event)
{
    if (is_button_event(event->type)) {
        const uint32_t elapsed_ms = event->tick_ms - state->last_switch_ms;
        if (elapsed_ms < BUTTON_DEBOUNCE_MS) {
            state->dropped_events++;
            return false;
        }

        const kvm_host_t target = event_target_host(event);
        state->last_switch_ms = event->tick_ms;
        if (target == state->active_host) {
            return false;
        }

        state->active_host = target;
        state->switch_count++;
        board_set_active_host_leds(target);
        board_beep_switch(target);
        kvm_slots_set_active(target);

        const esp_err_t sel_err = ble_mouse_select_host(target);
        if (sel_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "switched UI to %s but no bond yet — long-press to pair",
                     kvm_host_name(target));
        } else {
            ESP_LOGI(TAG, "active host switched to %s (switch_count=%" PRIu32 ")",
                     kvm_host_name(target), state->switch_count);
        }
        return true;
    }

    switch (event->type) {
    case KVM_EVENT_PAIR_LEFT:
    case KVM_EVENT_PAIR_RIGHT:
        ESP_LOGI(TAG, "pairing requested for %s", kvm_host_name(event->host));
        state->active_host = event->host;
        board_set_active_host_leds(event->host);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ble_mouse_start_pairing(event->host));
        return true;
    case KVM_EVENT_USB_HOST_STARTED:
        state->usb_host_started = true;
        ESP_LOGI(TAG, "USB host started");
        return true;
    case KVM_EVENT_USB_DEVICE_SEEN:
        state->usb_device_seen = true;
        state->usb_vid = event->usb_vid;
        state->usb_pid = event->usb_pid;
        ESP_LOGI(TAG, "USB device seen VID=0x%04X PID=0x%04X", state->usb_vid, state->usb_pid);
        return true;
    case KVM_EVENT_USB_HID_INTERFACE_SEEN:
        state->usb_device_seen = true;
        state->usb_hid_seen = true;
        state->usb_vid = event->usb_vid;
        state->usb_pid = event->usb_pid;
        state->usb_proto = event->usb_proto;
        ESP_LOGI(TAG,
                 "USB HID interface seen VID=0x%04X PID=0x%04X proto=%u",
                 state->usb_vid,
                 state->usb_pid,
                 state->usb_proto);
        return true;
    case KVM_EVENT_USB_MOUSE_CONNECTED:
        state->usb_device_seen = true;
        state->usb_hid_seen = true;
        state->usb_mouse_connected = true;
        if (event->usb_vid != 0 || event->usb_pid != 0) {
            state->usb_vid = event->usb_vid;
            state->usb_pid = event->usb_pid;
        }
        state->usb_proto = event->usb_proto;
        board_set_usb_status(true);
        return true;
    case KVM_EVENT_USB_MOUSE_DISCONNECTED:
        state->usb_mouse_connected = false;
        state->usb_hid_seen = false;
        board_set_usb_status(false);
        return true;
    case KVM_EVENT_USB_MOUSE_REPORT:
        state->mouse_report_count++;
        state->last_mouse_report_ms = event->tick_ms;
        if (event->mouse_dx != 0 || event->mouse_dy != 0 || event->mouse_wheel != 0 || event->mouse_buttons != state->last_mouse_buttons) {
            state->last_mouse_dx = event->mouse_dx;
            state->last_mouse_dy = event->mouse_dy;
            state->last_mouse_wheel = event->mouse_wheel;
            state->last_mouse_buttons = event->mouse_buttons;
        }
        state->last_mouse_raw_len = event->mouse_raw_len;
        memcpy(state->last_mouse_raw, event->mouse_raw, sizeof(state->last_mouse_raw));
        return true;
    case KVM_EVENT_BLE_HOST_CONNECTED:
        state->ble_connected[event->host] = true;
        ESP_LOGI(TAG, "BLE host connected: %s", kvm_host_name(event->host));
        return true;
    case KVM_EVENT_BLE_HOST_DISCONNECTED:
        state->ble_connected[event->host] = false;
        ESP_LOGW(TAG, "BLE host disconnected: %s", kvm_host_name(event->host));
        return true;
    case KVM_EVENT_WATCHDOG_TICK:
        return false;
    default:
        state->dropped_events++;
        return false;
    }
}
