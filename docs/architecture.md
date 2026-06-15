# Architecture

Visual overview of how the firmware, the host laptops, and the companion daemon co-operate. All diagrams here render natively on GitHub via Mermaid.

For the binary wire protocol of the custom GATT service used by the companion, see [`gatt-contract.md`](gatt-contract.md). For non-obvious design decisions and gotchas, see [`../AGENTS.md`](../AGENTS.md).

## System overview

```mermaid
flowchart LR
  receiver([USB wireless mouse receiver])
  esp[[ESP32-S3 firmware]]
  oled[(SSD1306 OLED)]
  btns([Buttons + LEDs + buzzer])

  subgraph LeftHost["Laptop A (slot LEFT)"]
    macHID([System BLE HID stack])
    macApp[Companion daemon]
  end

  subgraph RightHost["Laptop B (slot RIGHT)"]
    winHID([System BLE HID stack])
    winApp[Companion daemon]
  end

  receiver -- USB HID --> esp
  esp -- I2C --> oled
  btns <-- GPIO --> esp

  esp -- BLE HID Mouse --- macHID
  esp -- BLE HID Mouse --- winHID
  esp -- KVM Control GATT --- macApp
  esp -- KVM Control GATT --- winApp
```

The ESP holds **two simultaneous BLE connections**, one to each host's identity address. Each host's OS subscribes to the standard HID service. The companion daemon on each host attaches to the same BLE link and talks to a custom GATT service used solely for edge events and cursor warp instructions.

## Firmware task layout

```mermaid
flowchart TB
  subgraph FreeRTOS
    btn[button_scan_task<br/>polled, debounced]
    usb[usb_host_task]
    kvm[kvm_task<br/>state machine]
    ui[ui_task<br/>OLED render]
    ble[ble_hid_task<br/>USB→BLE bridge]
    nimble[NimBLE host task]
  end

  q1{{g_kvm_event_queue}}
  q2{{g_mouse_report_queue}}
  state[(kvm_state_t)]
  conn[(s_conn_handle[2]<br/>s_hid_subscriber_conn[2]<br/>s_desired_slot)]

  btn -- KVM_EVENT_BUTTON_* --> q1
  btn -- KVM_EVENT_PAIR_* --> q1
  usb -- USB_MOUSE_* --> q1
  usb -- mouse_report_t --> q2
  q1 --> kvm
  kvm --> state
  kvm --> conn
  state --> ui
  q2 --> ble
  ble -- ble_gatts_notify_custom --> nimble
  conn -.read.- ble

  gattcb[GATT access callbacks<br/>ble_control.c]
  gattcb -- KVM_EVENT_GATT_SWITCH_* --> q1
```

- Buttons and USB Host events fan into a single state queue; the `kvm_task` is the only writer to `kvm_state_t`.
- Mouse reports go through a separate queue so the BLE-side bridge can coalesce and rate-limit them without blocking USB.
- The custom GATT service callbacks (`src/ble_control.c`) post the same `KVM_EVENT_GATT_SWITCH_*` events that the manual button path uses, so an auto-switch updates LEDs / buzzer / display / NVS exactly like a manual switch.

## Pairing flow

```mermaid
sequenceDiagram
  participant U as User
  participant B as Buttons
  participant K as kvm_state
  participant M as ble_mouse
  participant N as NimBLE
  participant H as Laptop (system BT)

  U->>B: Long-press LEFT (≥1.5 s)
  B->>K: KVM_EVENT_PAIR_LEFT
  K->>M: ble_mouse_start_pairing(LEFT)
  M->>M: kvm_slots_clear(LEFT)<br/>ble_store_util_delete_peer(LEFT)
  M->>N: ble_hs_id_set_rnd(identity_LEFT)
  M->>N: ble_gap_adv_start(undirected connectable)
  H->>N: scan + connect + pair
  N-->>M: BLE_GAP_EVENT_ENC_CHANGE (bonded=1)
  M->>M: kvm_slots_set(LEFT, peer_addr)
  M->>K: post KVM_EVENT_BLE_HOST_CONNECTED
  K->>K: ble_connected[LEFT]=true
  M->>N: update_advertising()<br/>(now broadcast on s_desired_slot)
```

After both slots have bonds, subsequent boots auto-reconnect each host under its own identity without any user action.

## Manual switching (button)

```mermaid
sequenceDiagram
  participant U as User
  participant B as Buttons
  participant K as kvm_state
  participant M as ble_mouse
  participant H1 as Laptop A
  participant H2 as Laptop B

  Note over M: Both slots already connected<br/>s_desired_slot = LEFT, mouse → H1
  U->>B: Short-press RIGHT
  B->>K: KVM_EVENT_BUTTON_RIGHT
  K->>K: active_host = RIGHT<br/>LEDs / beep / NVS
  K->>M: ble_mouse_select_host(RIGHT)
  M->>M: s_desired_slot = RIGHT
  Note over M: send_report now targets<br/>s_hid_subscriber_conn[RIGHT]
  M-->>H2: HID notify(dx, dy, ...)
  Note over H1: Stops receiving notifies<br/>(cursor freezes where it was)
```

No BLE re-negotiation happens during the switch — both links stayed live, the firmware simply changes which `conn_handle` it notifies. Latency is bounded by a single GATT notification.

## Auto switching via companion (edge crossing)

```mermaid
sequenceDiagram
  participant Cur as Cursor on A
  participant CA as Companion (A)
  participant E as ESP firmware
  participant CB as Companion (B)
  participant CurB as Cursor on B

  loop ~60 Hz
    CA->>CA: cursor::current_position()
    Cur-->>CA: (x, y)
    alt x near right edge AND armed
      CA->>E: GATT write edge_event(Right, y_norm)
      E->>E: ble_mouse_force_select(RIGHT)<br/>posts KVM_EVENT_GATT_SWITCH_TO_RIGHT
      E->>E: kvm_state: active_host=RIGHT<br/>LEDs/beep/display/NVS
      E->>CB: GATT notify warp_cmd(FromLeft, y_norm)
      CB->>CurB: SetCursorPos(0, y_norm * height_B)
    end
  end
```

The two companions never talk to each other directly. Everything is mediated by the firmware, which is the only piece that knows the slot mapping (which laptop is on which "side") and the cursor state on both hosts (via the edge_event / warp_cmd characteristics).

## Connection state machine inside `ble_mouse.c`

```mermaid
stateDiagram-v2
  [*] --> Booting
  Booting --> Silent: no bonds at all
  Booting --> Reconnecting: any slot has bond
  Silent --> Pairing: long-press → start_pairing
  Reconnecting --> OneConnected: HID stack connects
  OneConnected --> BothConnected: other host's HID stack connects
  OneConnected --> Reconnecting: peer drops
  BothConnected --> Broadcasting: non-connectable adv on s_desired_slot
  Broadcasting --> OneConnected: any peer drops
  Pairing --> OneConnected: ENC_CHANGE bonded=1
  Pairing --> Silent: 10 fails → bailout

  state Reconnecting {
    [*] --> AdvertisingForFreeSlot
    AdvertisingForFreeSlot: connectable adv<br/>with whitelist for bonded peer
  }
```

`update_advertising()` is the single entry point for all advertising state changes — every transition above goes through it. Calling `ble_gap_adv_start` / `ble_gap_adv_stop` from anywhere else in the codebase is a bug.
