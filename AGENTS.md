# AGENTS.md

Notes for AI coding agents (Claude Code, Codex, Cursor, etc.) working on this firmware. Read this before making changes — it captures non-obvious design decisions and historical pitfalls that the file structure alone won't tell you.

## What this project is

ESP32-S3 firmware (ESP-IDF, NimBLE host, PlatformIO) for a BLE mouse KVM switch. A wireless USB mouse plugs into the ESP32-S3's USB Host port. The ESP32 acts as a BLE HID Mouse peripheral and bridges USB HID reports onto BLE. Two laptops can be paired simultaneously; a button press flips which one receives the input.

See [README.md](README.md) for the user-facing overview and pinout.

## Working environment

- **Build**: `pio run`. Don't try `idf.py` — this is a PlatformIO project.
- **Flash**: `pio run -t upload`. **Erase NVS** with `pio run -t erase` only when bond/identity state needs wiping (rarely). NVS persistence is load-bearing for UX.
- **Logs**: `pio device monitor`. Logs live in `/Users/ikorg/Downloads/mouse_logs.txt` during interactive debugging sessions — the user appends them there manually.
- **sdkconfig**: edit `sdkconfig.defaults` for durable changes, but also patch the generated `sdkconfig.esp32-s3-devkitc-1-n16r8v` so the current build picks them up without a full reconfigure. Both files are committed.

## Hard-won design decisions (don't undo these without reason)

### Buttons are active HIGH with internal pull-down

`BUTTON_PRESSED_LEVEL = 1`, `pull_down_en = GPIO_PULLDOWN_ENABLE`. An earlier attempt assumed active LOW with pull-up; the line floated after release and ESP read sustained "press" for 6–25 seconds. Don't revert.

The button task is a polled debouncer (3 consecutive samples = 60 ms stable). No GPIO ISR — the ISR-on-press-only design generated double events when the user held the button across the long-press threshold. Short press fires on **release** (only if long-press did not fire). Long-press fires after `BUTTON_LONG_PRESS_MS` (1500 ms) of stable contact, plus a distinct double-beep.

`BUTTON_STUCK_PRESS_MS` (6000 ms) guards against mechanical sticking: any "press" longer than that is ignored entirely. `armed` flag suppresses any session that starts already-pressed at boot.

### Dual BLE identity per slot

Each slot has its own random static BLE address (generated once via `ble_hs_id_gen_rnd`, stored in NVS keys `iL` / `iR`). Before advertising for a slot, the code calls `ble_hs_id_set_rnd(identity.val)` and uses `BLE_OWN_ADDR_RANDOM`.

**Why it matters**: a laptop bonded to ESP under identity LEFT will not auto-connect when ESP advertises under identity RIGHT. This was the only reliable way to keep the inactive host from hijacking pairing or reconnect of the other slot.

NimBLE bond store keys bonds by **peer** address, not local address, so reconnect to the right slot still works.

### Two simultaneous BLE connections

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`. State is per-slot: `s_conn_handle[2]`, `s_secure[2]`. `update_advertising()` advertises for whichever slot is currently disconnected (with whitelist + slot's identity), or in pairing mode if a long-press is pending. Both slots can connect; their `conn_handle`s are stored side by side.

**Crucial**: `esp_hidd_dev_input_set` from the ESP-IDF HIDD wrapper only notifies one internally-tracked `conn_id`, so it can't direct notifications. `ble_mouse_send_report` bypasses it and calls `ble_gatts_notify_custom(conn, attr, om)` directly with the active slot's `conn_handle`. The HID Input Report attribute handle (`s_hid_report_attr`) is captured opportunistically from `BLE_GAP_EVENT_SUBSCRIBE` (largest `notify`-subscribed handle).

If you ever extend the HID GATT service with additional notify-able characteristics, the "largest handle wins" heuristic might capture the wrong attribute. Make it explicit then (track by characteristic UUID).

### Slot mapping is independent of NimBLE bond store

`kvm_slots` (NVS namespace) maps `slot -> { peer identity address, local identity address }` plus the persisted "last active slot". NimBLE's own bond store maps `peer addr -> LTK/IRK`. Both are needed.

- `kvm_slots_lookup(addr, &slot)` resolves which slot owns a given peer.
- `kvm_slots_prune_orphan_bonds()` runs at startup and deletes NimBLE bonds whose peer is not in `kvm_slots` — important after firmware-level changes that wiped slot mapping but left bonds in the store.
- `kvm_slots_set(host, addr)` enforces a single peer per slot: if the same address is already in the other slot, it gets cleared there first.

### Pairing flow guarantees

`ble_mouse_start_pairing(host)`:

1. Removes only **this** slot's NimBLE bond (if any) and `kvm_slots_clear(host)`. The other slot is untouched.
2. Sets `s_pending_pair_slot`, `s_pair_fail_count = 0`.
3. If the slot's current `conn_handle` is active, terminates only that connection.
4. `update_advertising()` will then advertise in pairing mode for this slot.

`BLE_GAP_EVENT_ENC_CHANGE` with a pending pair:

- If the encrypted peer is already in a *different* slot, this is the case where a still-bonded laptop auto-connected during pairing. The intent is cancelled and the connection accepted as a regular reconnect to the slot it actually belongs to.
- Otherwise, the new bond is stored for `s_pending_pair_slot`.

Pair-fail bailout (10 consecutive failed disconnects in a pairing session) prevents endless retry loops when a laptop holds an obsolete LTK.

## Things that look like bugs but aren't

- **Mac shows multiple "ESP32 KVM Mouse" in Nearby Devices**. Each `pio run -t erase` regenerates identities; the Mac's BT scanner caches sightings. Tell the user to forget stale ones in System Settings.
- **`pio device monitor` connects after some delay** and misses early boot logs. Suggest starting the monitor before/right at flash time when boot-time behavior is being debugged.
- **`BLE connect failed status=22`** then immediate `BLE HID connected` in logs. NimBLE quirk — the connect status code is delivered separately from the wrapper-level callback; ignore if encryption follows.

## Don't do

- Don't reintroduce `esp_hidd_dev_input_set` for mouse reports. It routes to the last-connected `conn_id` and breaks the dual-host model.
- Don't use a single shared identity for both slots — see "Dual BLE identity per slot".
- Don't clear the whole NimBLE bond store on long-press. Only clear the targeted slot's bond.
- Don't bring back the GPIO press-edge ISR for buttons. Polling-only debounce is what works on the actual hardware here.
- Don't auto-Persist `s_desired_slot` from a peer that auto-connected without user intent — only persist on a real successful pairing/reconnect under user action.

## Useful invariants to check after changes

- `s_conn_handle[slot] != NONE` implies `s_secure[slot]` follows (true once encryption completes, false on disconnect).
- `update_advertising()` is the only function that calls `ble_gap_adv_start` / `ble_gap_adv_stop`. Every state transition (connect, disconnect, ENC_CHANGE, select_host, start_pairing) ends with a call to it.
- `ble_mouse_send_report` returns `ESP_ERR_INVALID_STATE` unless the desired slot is connected AND secure AND the HID attr is captured. UI should not show "WORK" mode before that.
- After `kvm_slots_clear(slot)`, the matching NimBLE bond should also be removed (`ble_store_util_delete_peer`) — otherwise reconnects will get stuck on an LTK mismatch.

## Where to look first

- BLE issues → `src/ble_mouse.c` (especially `ble_mouse_gap_event` and `update_advertising`).
- Button feel issues → `src/board_io.c` (`button_scan_task`).
- NVS / persistence issues → `src/kvm_slots.c`.
- "Why does the display say X" → `src/tasks.c` (`device_mode_text`, `render_status`).

## Sanity-test sequence after non-trivial BLE changes

1. `pio run -t erase && pio run -t upload`, start monitor immediately.
2. Long-press LEFT, pair on laptop A. Expect `generated identity for slot left: ...` once, `slot left bound to ...`, `HID report attr captured: NN`.
3. Long-press RIGHT, pair on laptop B. Expect a **different** identity MAC and a different bond address.
4. Move mouse — works on whichever slot is `s_desired_slot`.
5. Short-press to switch — mouse moves to the other host within ~10 ms, no `disconnected` log line in between.
6. Reset ESP32 — both hosts auto-reconnect under their own identities without user action.
