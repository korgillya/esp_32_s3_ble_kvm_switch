#pragma once

#include <stdint.h>

typedef enum {
    KVM_HOST_LEFT = 0,
    KVM_HOST_RIGHT = 1,
} kvm_host_t;

typedef enum {
    KVM_EVENT_BUTTON_LEFT,
    KVM_EVENT_BUTTON_RIGHT,
    KVM_EVENT_PAIR_LEFT,
    KVM_EVENT_PAIR_RIGHT,
    KVM_EVENT_USB_HOST_STARTED,
    KVM_EVENT_USB_DEVICE_SEEN,
    KVM_EVENT_USB_HID_INTERFACE_SEEN,
    KVM_EVENT_USB_MOUSE_CONNECTED,
    KVM_EVENT_USB_MOUSE_DISCONNECTED,
    KVM_EVENT_USB_MOUSE_REPORT,
    KVM_EVENT_BLE_HOST_CONNECTED,
    KVM_EVENT_BLE_HOST_DISCONNECTED,
    KVM_EVENT_GATT_SWITCH_TO_LEFT,
    KVM_EVENT_GATT_SWITCH_TO_RIGHT,
    KVM_EVENT_WATCHDOG_TICK,
} kvm_event_type_t;

typedef struct {
    kvm_event_type_t type;
    kvm_host_t host;
    uint32_t tick_ms;
    uint16_t usb_vid;
    uint16_t usb_pid;
    uint8_t usb_proto;
    int8_t mouse_dx;
    int8_t mouse_dy;
    int8_t mouse_wheel;
    uint8_t mouse_buttons;
    uint8_t mouse_raw_len;
    uint8_t mouse_raw[8];
} kvm_event_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    int8_t wheel;
    uint8_t buttons;
    uint8_t raw_len;
    uint8_t raw[8];
} mouse_report_t;
