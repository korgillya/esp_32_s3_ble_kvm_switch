# ESP32 KVM Companion

Cross-platform daemon (macOS + Windows, Linux possible) that pushes edge-crossing events from the host OS into the ESP32-S3 BLE KVM switch and warps the cursor when the active host changes.

## How it fits

```
        cursor at right edge of macOS screen
                       │
              GetCursorPosition / CGEventGetLocation
                       │
                  edge.rs detector
                       │
             write edge_event characteristic
                       │
                       ▼
              ┌──────────────────┐
              │  ESP32-S3        │
              │  KVM Control GATT│  ← single source of truth
              └──────────────────┘
                       │
            notify warp_cmd to other slot
                       │
                       ▼
            cursor.rs warp via SetCursorPos / CGWarpMouseCursorPosition
```

See [../docs/gatt-contract.md](../docs/gatt-contract.md) for the wire protocol.

## Build & run

```bash
cd companion
cargo build --release
./target/release/esp32-kvm-companion
```

First run: the daemon scans for `ESP32 KVM Mouse`, connects, reads its slot id, and starts polling the cursor. On subsequent runs it remembers the peripheral by id (config dir TBD).

## Module layout

```
src/
├── main.rs          # CLI / tokio runtime / lifecycle
├── ble.rs           # btleplug client + KVM Control service operations
├── edge.rs          # cursor polling + edge crossing state machine
├── cursor.rs        # platform-abstracted read/write cursor
└── platform/
    ├── mod.rs       # cfg-gated re-export
    ├── macos.rs     # CGEventGetLocation / CGWarpMouseCursorPosition
    └── windows.rs   # GetCursorPos / SetCursorPos
```

## Status

Skeleton with placeholder logic. Implements:

- [x] CLI argument parsing
- [x] Tracing log setup
- [x] BLE discovery & connect by device name
- [x] Read `slot_id` characteristic
- [x] Subscribe to `warp_cmd` and dispatch to platform cursor setter
- [x] Cursor read abstraction (macOS, Windows)
- [ ] Edge detection state machine (TODO)
- [ ] Screen bounds discovery (TODO)
- [ ] Reconnect on link loss
- [ ] OS service / launchd / autostart packaging

## Permissions

- **macOS**: the daemon polls cursor position via Quartz; that does not require accessibility permission. Warping the cursor is also unprivileged. If we ever switch to a `CGEventTap` for lower-latency edge detection, Accessibility will be required.
- **Windows**: cursor read/write through `user32.dll` works without elevation. AV may flag the binary if we ever add a low-level mouse hook; polling avoids this.

## Pairing assumption

The companion daemon assumes the user has already paired the ESP32 with this OS through the system Bluetooth settings (so HID works). The daemon then re-uses that paired link for the custom KVM Control service.
