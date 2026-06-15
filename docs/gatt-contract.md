# KVM Control GATT Contract

Custom GATT service exposed by the ESP32-S3 alongside the standard HID Service. Companion apps on macOS and Windows use it to push edge-crossing events to the firmware and to receive cursor warp commands.

All UUIDs are 128-bit, custom-allocated. Multi-byte integers are **little-endian**.

## Service: KVM Control

| | |
| --- | --- |
| UUID | `6b766d63-6f6e-7472-6f6c-000000000001` |
| Ascii hint | `kvmcontrol` followed by 8 nibbles |

Each bonded host (LEFT slot, RIGHT slot) accesses the same service over its own connection. The firmware identifies the calling slot from `conn_handle` internally; the companion app never needs to pass a slot id.

### Characteristic: `slot_id` (read)

| | |
| --- | --- |
| UUID | `6b766d63-6f6e-7472-6f6c-000000000002` |
| Properties | READ |
| Value length | 1 byte |
| Payload | `0` = LEFT slot, `1` = RIGHT slot |

The companion app reads this once after connect/subscribe to learn which slot it represents. Used for logging and for the `edge_event` payload semantics below ("right edge of LEFT" maps to slot RIGHT, etc.).

### Characteristic: `edge_event` (write)

| | |
| --- | --- |
| UUID | `6b766d63-6f6e-7472-6f6c-000000000003` |
| Properties | WRITE, WRITE_NO_RSP |
| Value length | 3 bytes |
| Byte 0 | `edge_code`: `1`=right edge, `2`=left edge, `3`=top edge, `4`=bottom edge |
| Bytes 1-2 | `position_norm`: 0..10000, fixed-point fraction of the orthogonal axis at the edge crossing (`y * 10000 / screen_height` for left/right edges, `x * 10000 / screen_width` for top/bottom) |

Companion app writes this when the cursor reaches an edge of the host's screen while moving outwards. Firmware responds by:

1. Resolving the neighbor slot (v1: hardcoded — LEFT slot is the left monitor, RIGHT is the right; only `right`/`left` edges affect switching).
2. Setting `s_desired_slot` to the neighbor slot (instant switch — no advertising needed because both are already connected).
3. Notifying the neighbor's `warp_cmd` characteristic with the entry-side payload (see below).

Top/bottom edges are reserved for future vertical layouts; v1 firmware ignores them silently.

### Characteristic: `warp_cmd` (notify)

| | |
| --- | --- |
| UUID | `6b766d63-6f6e-7472-6f6c-000000000004` |
| Properties | NOTIFY |
| Value length | 3 bytes |
| Byte 0 | `entry_side`: `1`=enter from left edge, `2`=enter from right edge, `3`=enter from top, `4`=enter from bottom |
| Bytes 1-2 | `position_norm`: 0..10000, fixed-point fraction of the orthogonal axis where the cursor should appear |

Firmware notifies this on the **new active** slot's connection right after handling an `edge_event`. Companion app subscribes during init and, on each notification, calls the OS API to warp the cursor:

- `entry_side=1` (from left) → set cursor to `x = 0`, `y = position_norm / 10000 * screen_height`.
- `entry_side=2` (from right) → set cursor to `x = screen_width - 1`, `y = position_norm / 10000 * screen_height`.

## Concrete example

User pushes mouse off the right edge of the macOS laptop at vertical position 540 / 1080 = 0.5.

1. macOS app polls cursor, detects right-edge crossing at `y_norm = 5000`.
2. macOS app writes `[0x01, 0x88, 0x13]` (right edge, 5000) to `edge_event`.
3. Firmware: macOS is slot LEFT → neighbor is slot RIGHT (Windows).
   - `s_desired_slot = RIGHT`.
   - Notify Windows's `warp_cmd` with `[0x01, 0x88, 0x13]` (enter from left edge, y_norm=5000).
4. Windows app receives notification, calls `SetCursorPos(0, 540)` (assuming 1080-tall screen).
5. Mouse motion now flows to Windows; cursor continues smoothly on the new screen.

## UUID stable encoding

The full UUID layout uses a single base UUID with a varying suffix counter:

```
6b766d63-6f6e-7472-6f6c-000000000XXX
```

| XXX | Purpose |
| --- | --- |
| 001 | Service |
| 002 | slot_id (read) |
| 003 | edge_event (write) |
| 004 | warp_cmd (notify) |

When extending: increment counter. Do not reuse retired UUIDs.

## Future characteristics (not in v1)

- `0...005` `screen_size` (write, 4 bytes): host pushes its screen dimensions on startup so firmware could implement smarter heuristics (e.g., warp using exact pixels, multi-monitor configs).
- `0...006` `layout` (read/write, 1 byte): physical adjacency override (`0` = LEFT-RIGHT, `1` = RIGHT-LEFT, ...). v1 hardcodes LEFT-RIGHT.
