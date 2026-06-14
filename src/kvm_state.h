#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_events.h"

typedef struct {
    kvm_host_t active_host;
    bool usb_host_started;
    bool usb_device_seen;
    bool usb_hid_seen;
    bool usb_mouse_connected;
    bool ble_connected[2];
    uint16_t usb_vid;
    uint16_t usb_pid;
    uint8_t usb_proto;
    uint32_t mouse_report_count;
    uint32_t last_mouse_report_ms;
    int8_t last_mouse_dx;
    int8_t last_mouse_dy;
    int8_t last_mouse_wheel;
    uint8_t last_mouse_buttons;
    uint8_t last_mouse_raw_len;
    uint8_t last_mouse_raw[8];
    uint32_t switch_count;
    uint32_t last_switch_ms;
    uint32_t dropped_events;
} kvm_state_t;

void kvm_state_init(kvm_state_t *state);
bool kvm_state_handle_event(kvm_state_t *state, const kvm_event_t *event);
const char *kvm_host_name(kvm_host_t host);
