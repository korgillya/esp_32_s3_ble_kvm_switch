# ESP32-S3 BLE KVM Switch

Firmware + companion app for an ESP32-S3 based BLE mouse KVM switch. The device reads a wireless mouse receiver through USB Host, holds **two simultaneous BLE HID connections** (one per laptop), and switches the active laptop either manually (button press) or **automatically when the cursor crosses a screen edge** (companion app sends an edge event over a custom GATT service).

Built on ESP-IDF + NimBLE (firmware, PlatformIO) and Rust with `bluest` (companion daemon).

## Status

- **Manual switching** between two paired laptops via short-press button — sub-10 ms latency, both hosts stay connected simultaneously.
- **Per-slot BLE identity addresses** prevent the inactive host from hijacking the other slot.
- **Bond-isolated pairing**: long-press LEFT/RIGHT only touches that slot's bond.
- **NVS persistence**: bond keys, identity addresses, last-active slot survive resets.
- **OLED status display** + LEDs + buzzer feedback.
- **Auto edge-switching (macOS)** ✅ — companion app on macOS detects cursor at screen edge, writes to ESP's custom KVM Control GATT service, ESP flips active slot and notifies the new host's companion to warp the cursor.
- **Auto edge-switching (Windows)** — same companion crate, untested on Windows yet.

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

Buttons read **active HIGH** (line driven by the press, idle pulled down by internal pull-down). See `BUTTON_PRESSED_LEVEL` in [`src/board_io.c`](src/board_io.c).

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
              -> ble_gatts_notify_custom on the HID-subscribing conn_handle

ui_task
  -> SSD1306 render from kvm_state_t

ble_control GATT service (slot_id / edge_event / warp_cmd)
  -> companion app writes edge_event
       -> ble_mouse_force_select(target_slot)
            -> posts KVM_EVENT_GATT_SWITCH_TO_* into kvm_state machine
       -> notifies warp_cmd on the new active slot's conn
```

### Key modules

| File | Responsibility |
| --- | --- |
| [`src/main.c`](src/main.c) | NVS init, queue creation, task startup. |
| [`src/tasks.c`](src/tasks.c) | `kvm_task`, `ui_task`, `ble_hid_task` bodies + USB-to-BLE mouse bridge. |
| [`src/kvm_state.c`](src/kvm_state.c) | State machine for button/USB/BLE/GATT events; updates active slot, counters, USB info. |
| [`src/kvm_slots.c`](src/kvm_slots.c) | NVS-backed mapping `slot -> { bonded peer addr, local random identity }`, active-slot persistence, orphan-bond pruning. |
| [`src/ble_mouse.c`](src/ble_mouse.c) | NimBLE peripheral. Two `conn_handle`s, per-slot advertising, HID Input Report notify via `ble_gatts_notify_custom` to the conn that subscribed. |
| [`src/ble_control.c`](src/ble_control.c) | Custom GATT service `kvm-control` — `slot_id` (read), `edge_event` (write), `warp_cmd` (notify). See [`docs/gatt-contract.md`](docs/gatt-contract.md). |
| [`src/board_io.c`](src/board_io.c) | GPIO/LED/buzzer init, debounced button scanner (short = switch, long = pair). |
| [`src/usb_mouse_host.c`](src/usb_mouse_host.c) | USB Host + HID parser, posts mouse reports to the queue. |
| [`src/ssd1306.c`](src/ssd1306.c) | Small SSD1306 OLED driver. |

### Dual-host BLE strategy

- ESP32 maintains **two random static BLE identities** per slot (generated once, stored in NVS namespace `kvm_slots`).
- Bonded peers see ESP under one specific identity, so the wrong host cannot autoconnect to the slot it does not own.
- `update_advertising()` chooses what to advertise on every state change:
  1. Pending pair slot (long-press) → pairing mode on that identity.
  2. Otherwise pick a free slot with a stored bond (preferring `s_desired_slot`) → connectable reconnect.
  3. If both slots are connected → non-connectable broadcast on `s_desired_slot` identity so companion apps can scan-discover the peripheral.
- HID Input Report attribute handle (`s_hid_report_attr`) is captured on the **first** notify-subscribe (the OS HID stack subscribes immediately after pairing; companion's later subscribes to other characteristics don't overwrite it).
- `s_hid_subscriber_conn[slot]` tracks **which conn** subscribed to HID Report — mouse reports go through that exact conn, not the latest slot conn, so a companion's second BLE link on the same peer doesn't hijack mouse output.

## Companion app

Cross-platform Rust daemon under [`companion/`](companion/). Uses `bluest` for BLE and platform-specific cursor APIs (`core-graphics` on macOS, `windows-rs` on Windows). See [`companion/README.md`](companion/README.md) for build instructions and [`docs/gatt-contract.md`](docs/gatt-contract.md) for the wire protocol.

Run on each laptop:

```bash
cd companion
cargo build --release
RUST_LOG=info cargo run --release
```

When both companions are running and slots are paired:
- Move cursor to the right edge of laptop A → ESP receives `edge_event(right)`, switches active slot to laptop B, notifies laptop B's companion with `warp_cmd(from_left, y_norm)` → cursor appears on the left edge of laptop B at the same vertical position.

## Build & flash firmware

```bash
pio run                  # build
pio run -t upload        # flash
pio device monitor       # serial logs

# Reset bonds and identities (rare):
pio run -t erase && pio run -t upload
```

## Pairing flow

1. Long-press LEFT (≥1.5 s, two short beeps) → pairing mode on slot LEFT identity. Pair "ESP32 KVM Mouse" in laptop A's Bluetooth settings.
2. Long-press RIGHT → same for slot RIGHT, advertised under a different MAC, paired on laptop B.
3. Both bonds stored; short-press switches between them. Both stay BLE-connected.
4. Optional: run the companion on each laptop to enable auto edge-switching.

## Roadmap

1. ~~Bring-up: buttons, LEDs, buzzer, I2C/OLED~~ ✅
2. ~~USB Host: detect mouse receiver and parse HID mouse reports~~ ✅
3. ~~BLE HID: expose the ESP32-S3 as a BLE mouse~~ ✅
4. ~~Bridge: route USB mouse reports to the selected BLE host~~ ✅
5. ~~Dual-host mode: pair and switch between two laptops~~ ✅
6. ~~Dual-connection mode: instant switching via per-conn notify routing~~ ✅
7. ~~Custom GATT service + companion app + auto edge-switching (macOS)~~ ✅
8. **Validate companion on Windows** — same code, untested.
9. Configurable physical layout (which slot is the left monitor).
10. Multi-monitor support per host.
11. Vertical edge adjacency (top/bottom).
12. Pre-built companion binaries + launchd / Windows Service autostart.

## Memory / build size

| Resource | Usage |
| --- | --- |
| Flash | ~66% of 1 MB app partition |
| RAM | ~10% of 320 KB |
