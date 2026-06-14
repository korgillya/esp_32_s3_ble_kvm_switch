# ESP32-S3 BLE KVM Switch

Firmware for an ESP32-S3 based BLE mouse KVM switch. The device reads a wireless mouse receiver through USB Host, holds **two simultaneous BLE HID connections** (one per laptop), and instantly routes mouse reports to the active host on a button press. Built on ESP-IDF + NimBLE via PlatformIO.

## Status

- **Manual switching between two paired laptops works end-to-end.** Both hosts stay connected at the same time; short-press just changes which `conn_handle` the firmware notifies. Switch latency is essentially a single GATT notification.
- **Per-slot BLE identity addresses** prevent the inactive host from auto-hijacking pairing for the other slot.
- **Pairing flow** is bond-isolated: long-press LEFT only touches the LEFT bond, RIGHT only touches RIGHT. Re-pairing one host does not lose the other.
- **NVS persistence**: bond keys, identity addresses per slot, and the last active slot survive resets. macOS / Windows reconnect without a "Forget Device" step.
- **OLED status display** shows active host, USB status, mouse counters, and connection mode.
- **Companion-app edge switching** is not implemented yet — see Roadmap.

## Hardware Pinout

GPIO19 and GPIO20 are reserved for native USB D-/D+ on ESP32-S3.

| Signal | GPIO | Notes |
| --- | ---: | --- |
| Left screen button | 4 | Active high, internal pull-down |
| Right screen button | 5 | Active high, internal pull-down |
| Left host LED | 6 | Active high |
| Right host LED | 7 | Active high |
| Buzzer PWM | 8 | LEDC PWM, distinct tones for switch vs pair |
| OLED SDA | 10 | I2C0 @ 400 kHz |
| OLED SCL | 11 | I2C0 |
| USB D- | 19 | Native USB Host |
| USB D+ | 20 | Native USB Host |

Buttons read **active HIGH** (line driven by the press, idle pulled down by internal pull-down). Earlier code assumed active LOW; if you wire your own buttons, double-check `BUTTON_PRESSED_LEVEL` in [`src/board_io.c`](src/board_io.c).

## Firmware Architecture

Four FreeRTOS tasks communicate through two queues:

```text
button_scan_task (polled, debounced)
  -> g_kvm_event_queue
       -> kvm_task -> kvm_state machine
              -> ble_mouse_select_host / start_pairing
              -> board_set_active_host_leds / beep
              -> updates kvm_state_t

usb_mouse_host_task
  -> g_mouse_report_queue
       -> ble_hid_task -> ble_mouse_send_report
              -> ble_gatts_notify_custom on the active slot's conn_handle

ui_task
  -> SSD1306 render from kvm_state_t
```

### Key modules

| File | Responsibility |
| --- | --- |
| [`src/main.c`](src/main.c) | NVS init, queue creation, task startup. |
| [`src/tasks.c`](src/tasks.c) | `kvm_task`, `ui_task`, `ble_hid_task` bodies + USB-to-BLE mouse bridge. |
| [`src/kvm_state.c`](src/kvm_state.c) | State machine for button/USB/BLE events; updates active slot, counters, USB info. |
| [`src/kvm_slots.c`](src/kvm_slots.c) | NVS-backed mapping `slot -> { bonded peer addr, local random identity }`, active-slot persistence, orphan-bond pruning. |
| [`src/ble_mouse.c`](src/ble_mouse.c) | NimBLE peripheral. Holds two `conn_handle`s, manages advertising per slot, custom HID Input Report notify. |
| [`src/board_io.c`](src/board_io.c) | GPIO/LED/buzzer init, debounced button scanner (short = switch, long = pair). |
| [`src/usb_mouse_host.c`](src/usb_mouse_host.c) | USB Host + HID parser, posts mouse reports to the queue. |
| [`src/ssd1306.c`](src/ssd1306.c) | Small SSD1306 OLED driver. |

### Dual-host BLE strategy

- ESP32 maintains **two random static BLE identities** (generated once, stored in NVS under namespace `kvm_slots`). Slot LEFT and slot RIGHT each have their own MAC.
- Bonded peers store ESP under one specific identity, so the wrong host cannot autoconnect to the slot it does not own.
- Advertising target is recomputed by `update_advertising()` on every state change:
  1. If a pairing intent is pending and that slot is free → advertise in pairing mode (no whitelist).
  2. Otherwise, pick a free slot with a stored bond, preferring `s_desired_slot`, and advertise with that slot's identity + whitelist filter.
  3. If both slots are connected → advertising stops.
- HID Input Report attribute handle is captured from `BLE_GAP_EVENT_SUBSCRIBE` (largest notify-subscribed handle wins).
- `ble_mouse_send_report` calls `ble_gatts_notify_custom(conn_handle, attr, om)` directly, bypassing the ESP-IDF HIDD wrapper (which only tracks a single `conn_id` internally). This is what makes switching instantaneous and prevents both hosts from receiving the same mouse motion.

## Build & flash

```bash
pio run                  # build
pio run -t upload        # flash
pio device monitor       # serial logs

# When the on-flash NVS schema changes (rare), or to reset bonds:
pio run -t erase && pio run -t upload
```

## Pairing flow

1. Long-press LEFT button (>= 1.5 s, you'll hear two short beeps) → ESP advertises slot LEFT identity in pairing mode. On the laptop, pair "ESP32 KVM Mouse".
2. Long-press RIGHT button → same for slot RIGHT, advertised under a different MAC, paired on the other laptop.
3. After both bonds exist, short presses switch which laptop receives mouse events. Both stay connected.
4. If you ever lose a bond on the laptop side (Forget Device), just long-press the corresponding button again. The other slot is untouched.

## Roadmap

1. ~~Bring-up: buttons, LEDs, buzzer, I2C/OLED~~ ✅
2. ~~USB Host: detect mouse receiver and parse HID mouse reports~~ ✅
3. ~~BLE HID: expose the ESP32-S3 as a BLE mouse~~ ✅
4. ~~Bridge: route USB mouse reports to the selected BLE host~~ ✅
5. ~~Dual-host mode: pair and switch between two laptops~~ ✅ (manual)
6. ~~Dual-connection mode: hold both BLE links simultaneously for instant switching~~ ✅
7. **Auto edge-switching via companion app + custom GATT** — next.
   - Companion apps on macOS/Windows report cursor crossings + screen resolution over a custom characteristic.
   - ESP changes `s_desired_slot` on edge events and emits a cursor "warp" series to the new host.
8. Better UI: pairing progress indicator, status banners when waiting on the other laptop.
9. Power management: deep sleep on idle, wake on USB activity.

## Memory / build size

| Resource | Usage |
| --- | --- |
| Flash | ~66% of 1 MB app partition |
| RAM | ~10% of 320 KB |
