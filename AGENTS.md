# AGENTS.md

Notes for AI coding agents (Claude Code, Codex, Cursor, etc.) working on this project. Read this before making changes — it captures non-obvious design decisions and historical pitfalls that the file structure alone won't tell you.

## What this project is

ESP32-S3 firmware (ESP-IDF, NimBLE host, PlatformIO) + cross-platform Rust companion daemon (under `companion/`) for a BLE mouse KVM switch. A wireless USB mouse plugs into the ESP32-S3's USB Host port. The ESP32 acts as a BLE HID Mouse peripheral and bridges USB HID reports onto BLE. Two laptops can be paired simultaneously; switching is done by short-press button **or** automatically when the user's cursor crosses a screen edge (companion writes to a custom GATT service).

See [README.md](README.md) for the user-facing overview and [`docs/gatt-contract.md`](docs/gatt-contract.md) for the wire protocol between firmware and companion.

## Working environment

- **Firmware build**: `pio run`. Don't try `idf.py` — this is a PlatformIO project.
- **Flash**: `pio run -t upload`. **Erase NVS** with `pio run -t erase` only when bond/identity state needs wiping. NVS persistence is load-bearing for UX.
- **Logs**: `pio device monitor`. The user appends serial logs to `/Users/ikorg/Downloads/mouse_logs.txt` during interactive debugging.
- **Companion build**: `cd companion && cargo build --release`. Companion logs go to `/Users/ikorg/Downloads/companion_logs.txt` during testing.
- **sdkconfig**: edit `sdkconfig.defaults` for durable changes, but also patch the generated `sdkconfig.esp32-s3-devkitc-1-n16r8v` so the current build picks them up without a full reconfigure. Both files are committed.

## Hard-won design decisions (don't undo these without reason)

### Buttons are active HIGH with internal pull-down

`BUTTON_PRESSED_LEVEL = 1`, `pull_down_en = GPIO_PULLDOWN_ENABLE`. An earlier attempt assumed active LOW with pull-up; the line floated after release and ESP read sustained "press" for 6–25 seconds. Don't revert.

The button task is a polled debouncer (3 consecutive samples = 60 ms stable). No GPIO ISR — the ISR-on-press-only design generated double events when the user held the button across the long-press threshold. Short press fires on **release** (only if long-press did not fire). Long-press fires after `BUTTON_LONG_PRESS_MS` (1500 ms) of stable contact, plus a distinct double-beep.

`BUTTON_STUCK_PRESS_MS` (6000 ms) guards against mechanical sticking; sessions starting already-pressed at boot are ignored via the `armed` flag.

### Dual BLE identity per slot

Each slot has its own random static BLE address (generated once via `ble_hs_id_gen_rnd`, stored in NVS keys `iL` / `iR`). Before advertising for a slot, the code calls `ble_hs_id_set_rnd(identity.val)` and uses `BLE_OWN_ADDR_RANDOM`.

**Why it matters**: a laptop bonded to ESP under identity LEFT will not auto-connect when ESP advertises under identity RIGHT. This was the only reliable way to keep the inactive host from hijacking pairing or reconnect of the other slot.

NimBLE bond store keys bonds by **peer** address, not local address, so reconnect to the right slot still works.

### Two simultaneous BLE connections + per-conn notify routing

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`. State is per-slot: `s_conn_handle[2]`, `s_secure[2]`, `s_hid_subscriber_conn[2]`. `update_advertising()` advertises for whichever slot is currently disconnected (with whitelist + slot's identity), or non-connectable broadcast on `s_desired_slot` when both slots are connected, or pairing mode if a long-press is pending.

**Crucial**: `esp_hidd_dev_input_set` from the ESP-IDF HIDD wrapper only notifies one internally-tracked `conn_id`, so it can't direct notifications. `ble_mouse_send_report` bypasses it and calls `ble_gatts_notify_custom(conn, attr, om)` directly with the **HID-subscribing** conn handle for the current slot, not just whatever conn was last resolved into the slot.

The reason for the explicit `s_hid_subscriber_conn[]` (separate from `s_conn_handle[]`) is that the same peer can hold **two BLE connections** to ESP at once: one used by the system HID stack and one opened by the companion app from a scan-discovered peripheral. Mouse reports must keep flowing to the HID-subscribing conn even after the companion's conn shows up on the same slot.

### HID Report attribute is captured ONCE

`s_hid_report_attr` is set on the **first** notify-subscribe seen after the GATT DB starts (which is always the OS HID stack subscribing to HID Input Report). An earlier "largest handle wins" heuristic broke once the companion added a custom characteristic whose handle was higher than HID Report — mouse reports started going to `warp_cmd` instead. Do not change to a greedy or last-write-wins approach without filtering by characteristic UUID.

### Slot mapping is independent of NimBLE bond store

`kvm_slots` (NVS namespace) maps `slot -> { peer identity address, local identity address }` plus the persisted "last active slot". NimBLE's own bond store maps `peer addr -> LTK/IRK`. Both are needed.

- `kvm_slots_lookup(addr, &slot)` resolves which slot owns a given peer.
- `kvm_slots_prune_orphan_bonds()` runs at startup and deletes NimBLE bonds whose peer is not in `kvm_slots`.
- `kvm_slots_set(host, addr)` enforces a single peer per slot: if the same address is already in the other slot, it gets cleared there first.

### Pairing flow guarantees

`ble_mouse_start_pairing(host)`:

1. Removes only **this** slot's NimBLE bond (if any) and `kvm_slots_clear(host)`. The other slot is untouched.
2. Sets `s_pending_pair_slot`, `s_pair_fail_count = 0`.
3. If the slot's current `conn_handle` is active, terminates only that connection.
4. `update_advertising()` will then advertise in pairing mode for this slot.

`BLE_GAP_EVENT_ENC_CHANGE` with a pending pair: if the encrypted peer is already in a *different* slot, that means a still-bonded laptop auto-connected during pairing. Intent is cancelled and the connection is accepted as a regular reconnect to the slot it actually belongs to. Pair-fail bailout (10 consecutive failed disconnects) prevents endless retry loops when a laptop holds an obsolete LTK.

### Auto edge-switching: GATT contract (`src/ble_control.c`)

Three characteristics under custom service `6b766d63-6f6e-7472-6f6c-000000000001`:

- `slot_id` (read, 1 byte) — companion reads on connect to learn whether it's the LEFT or RIGHT host's app.
- `edge_event` (write, 3 bytes) — companion writes `[edge_code, y_norm_lo, y_norm_hi]` when cursor pushes a screen edge.
- `warp_cmd` (notify, 3 bytes) — ESP notifies the **other** host's companion to warp the cursor on entry.

Edge processing in firmware calls `ble_mouse_force_select(target_slot)`, which posts a `KVM_EVENT_GATT_SWITCH_TO_*` event into the kvm_state machine — the same event the manual button path emits. That keeps active slot + LEDs + buzzer + display + NVS persistence in sync regardless of whether the switch came from a button or from an edge crossing.

See [`docs/gatt-contract.md`](docs/gatt-contract.md) for the binary layout.

## Companion app (Rust, under `companion/`)

The companion uses `bluest` for BLE central and platform-specific cursor APIs (`core-graphics` on macOS, `windows-rs` on Windows). Several non-obvious things were required to make BLE work on macOS; the same shape will likely work on Windows out of the box but is not yet validated.

### macOS BLE quirks the companion has to work around

1. **macOS hides bonded HID peripherals from `scanForPeripheralsWithServices`** for user-space apps. We could not rely on `Adapter::connected_devices_with_services` either — it only returns peripherals whose service UUIDs have already been GATT-discovered by the OS, and our custom KVM Control service hasn't. The peripheral is therefore found via the `scan()` path filtered by the KVM service UUID (ESP advertises this UUID in its scan response and, when broadcasting, in the primary adv data).
2. **`bluest::Device` returned from a scan keeps its internal state as "discovered" even after `Adapter::connect_device` succeeds**. Subsequent reads/writes/notify-subscribes through that handle fail with `device isn't connected`. The fix is to call `Adapter::open_device(&device.id())` after `connect_device` to get a fresh handle backed by CoreBluetooth's retrievePeripheralsWithIdentifiers path. The current `ble.rs` does this and falls back to `connected_devices()` if `open_device` is unavailable.
3. **`bluest::Characteristic::notify()` returns a stream with a `'_` lifetime tied to the characteristic.** The stream must outlive the borrow. We `Box::leak(Box::new(characteristic.clone()))` to get a `&'static Characteristic` and store the leaked reference — one-time leak of a few bytes per characteristic per process. Same trick for `edge_event_chr` because plain `Arc<Characteristic>` clones still hit the same "device isn't connected" path on write.
4. **`#[tokio::main(flavor = "current_thread")]`** is required. CoreBluetooth dispatches its delegate callbacks onto a serial dispatch queue and bluest tracks state assuming task affinity; a multi-threaded tokio runtime moves awaits across worker threads and BLE writes start failing. The single-thread runtime also keeps everything (GATT, cursor polling, edge detection, warp processing) in one `tokio::select!` loop — no `tokio::spawn` for the BLE work.
5. **Keepalive reconnect** every 2 s. If we don't, CoreBluetooth seems to put the link into a power-saving state after a few seconds of inactivity and `write_without_response` starts failing. `ble::ensure_connected` calls `adapter.connect_device` if `device.is_connected()` returns false.

These are macOS-only mitigations; on Windows the WinRT BluetoothLE API surface is different and most of them should be no-ops. Don't strip them without testing on macOS first.

### Companion layout

```
companion/
├── Cargo.toml              # bluest, tokio (current_thread), core-graphics (mac), windows-rs (win)
└── src/
    ├── main.rs             # CLI, runtime, the single tokio::select! loop
    ├── ble.rs              # connect / find_device / send_edge_event / handle_warp_value / ensure_connected
    ├── edge.rs             # cursor polling + edge detection state machine
    ├── cursor.rs           # platform-abstracted read/write + screen bounds
    └── platform/
        ├── mod.rs          # cfg-gated re-export
        ├── macos.rs        # CGEventGetLocation / CGWarpMouseCursorPosition
        └── windows.rs      # GetCursorPos / SetCursorPos
```

`uuid` is pinned to `=1.11` because newer versions need Rust 1.85+ and most users still have stable 1.82.

## Things that look like bugs but aren't

- **Mac shows multiple "ESP32 KVM Mouse" in Nearby Devices**. Each `pio run -t erase` regenerates identities; the Mac's BT scanner caches sightings. Tell the user to forget stale ones in System Settings.
- **`pio device monitor` connects after some delay** and misses early boot logs. Start the monitor before/right at flash time when boot-time behavior is being debugged.
- **`BLE connect failed status=22`** then immediate `BLE HID connected` in logs. NimBLE quirk — the connect status code is delivered separately from the wrapper-level callback; ignore if encryption follows.
- **Companion logs `subscribed to warp_cmd` then ESP shows `attr=53 notify=0` ~20 ms later** when there's another BLE quirk (e.g. wrong scan filter or thread affinity issue). If subscribe sticks, the second `notify=0` should not appear; if it does, look at bluest configuration before touching firmware.
- **Companion log shows `right edge hit` 30+ times per second on the same y_norm**: that's expected — the cursor stays inside the 1-pixel edge band, the rearm logic prevents repeat sends until the cursor moves at least `REARM_PX` away.

## Don't do

- Don't reintroduce `esp_hidd_dev_input_set` for mouse reports.
- Don't use a single shared identity for both slots.
- Don't clear the whole NimBLE bond store on long-press.
- Don't bring back the GPIO press-edge ISR for buttons.
- Don't update `s_hid_report_attr` with anything other than the first observed notify-subscribe.
- Don't run companion on a multi-threaded tokio runtime — see macOS quirks above.
- Don't drop the `Box::leak` on companion characteristics until bluest's lifetime story for `notify()` and `write_without_response` changes.

## Useful invariants to check after changes

- `s_conn_handle[slot] != NONE` implies `s_secure[slot]` follows (true once encryption completes, false on disconnect).
- `update_advertising()` is the only function that calls `ble_gap_adv_start` / `ble_gap_adv_stop`. Every state transition (connect, disconnect, ENC_CHANGE, select_host, start_pairing) ends with a call to it.
- `ble_mouse_send_report` returns `ESP_ERR_INVALID_STATE` unless the desired slot has a HID subscriber AND it's secure AND the HID attr is captured.
- After `kvm_slots_clear(slot)`, the matching NimBLE bond should also be removed (`ble_store_util_delete_peer`).

## Where to look first

- BLE issues → `src/ble_mouse.c` (especially `ble_mouse_gap_event` and `update_advertising`).
- Custom GATT issues → `src/ble_control.c`.
- Button feel issues → `src/board_io.c` (`button_scan_task`).
- NVS / persistence issues → `src/kvm_slots.c`.
- "Why does the display say X" → `src/tasks.c` (`device_mode_text`, `render_status`).
- Companion connection / GATT issues → `companion/src/ble.rs`.
- Companion edge / warp issues → `companion/src/edge.rs` and `companion/src/cursor.rs`.

## Sanity-test sequence after non-trivial changes

1. `pio run -t erase && pio run -t upload`, start monitor immediately.
2. Long-press LEFT, pair on laptop A. Expect `generated identity for slot left: ...` once, `slot left bound to ...`, `HID report attr captured: NN`.
3. Long-press RIGHT, pair on laptop B. Expect a **different** identity MAC.
4. Move mouse — works on whichever slot is `s_desired_slot`.
5. Short-press to switch — mouse moves to the other host within ~10 ms, no `disconnected` log line in between.
6. Reset ESP32 — both hosts auto-reconnect under their own identities without user action.
7. `cd companion && cargo run --release` on each laptop. Expect `resolved slot ...` matching that host's slot.
8. Move cursor to the outward edge of laptop A. Expect `edge_event sent` in companion, `edge ... switch to ...` in firmware, LED + buzzer + display update on ESP, and (on laptop B) `warp_cmd received` + cursor jumps to the corresponding entry edge.
