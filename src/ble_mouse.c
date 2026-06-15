#include "ble_mouse.h"

#include <string.h>

#include "app_queues.h"
#include "ble_control.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "kvm_slots.h"
#include "kvm_state.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble-mouse";

#define BLE_MOUSE_NAME "ESP32 KVM Mouse"
#define BLE_HID_SERVICE_UUID 0x1812
#define BLE_ADV_CHANNEL_ALL 0x07
#define SLOT_NONE (-1)
#define PAIR_FAIL_BAILOUT 10
#define BROADCAST_DURATION_MS 2000

static esp_hidd_dev_t *s_hid_dev;
static bool s_started;
static bool s_advertising;
static bool s_pairing_mode;
static bool s_advertising_broadcast;
static int s_advertising_for_slot = SLOT_NONE;

static uint16_t s_conn_handle[2] = {BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE};
static bool s_secure[2] = {false, false};

/* The conn_handle that subscribed to the HID Input Report characteristic for
 * this slot. May differ from s_conn_handle[] when a companion app opens a
 * second BLE connection (e.g. macOS CoreBluetooth creates a separate conn
 * from a scan-discovered peripheral on top of the existing HID-paired one).
 * Mouse reports must go to this conn, not the latest-resolved one. */
static uint16_t s_hid_subscriber_conn[2] = {BLE_HS_CONN_HANDLE_NONE,
                                            BLE_HS_CONN_HANDLE_NONE};

static uint16_t s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int s_pending_conn_slot = SLOT_NONE;
static int s_pending_pair_slot = SLOT_NONE;
static int s_pair_fail_count;

static kvm_host_t s_desired_slot = KVM_HOST_LEFT;
static uint16_t s_hid_report_attr;

void ble_store_config_init(void);

static void update_advertising(void);

static const uint8_t s_mouse_report_map[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x02, // Usage (Mouse)
    0xA1, 0x01, // Collection (Application)
    0x09, 0x01, //   Usage (Pointer)
    0xA1, 0x00, //   Collection (Physical)
    0x05, 0x09, //     Usage Page (Button)
    0x19, 0x01, //     Usage Minimum (Button 1)
    0x29, 0x03, //     Usage Maximum (Button 3)
    0x15, 0x00, //     Logical Minimum (0)
    0x25, 0x01, //     Logical Maximum (1)
    0x95, 0x03, //     Report Count (3)
    0x75, 0x01, //     Report Size (1)
    0x81, 0x02, //     Input (Data,Var,Abs)
    0x95, 0x01, //     Report Count (1)
    0x75, 0x05, //     Report Size (5)
    0x81, 0x03, //     Input (Const,Var,Abs)
    0x05, 0x01, //     Usage Page (Generic Desktop)
    0x09, 0x30, //     Usage (X)
    0x09, 0x31, //     Usage (Y)
    0x09, 0x38, //     Usage (Wheel)
    0x15, 0x81, //     Logical Minimum (-127)
    0x25, 0x7F, //     Logical Maximum (127)
    0x75, 0x08, //     Report Size (8)
    0x95, 0x03, //     Report Count (3)
    0x81, 0x06, //     Input (Data,Var,Rel)
    0xC0,       //   End Collection
    0xC0,       // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_mouse_report_map,
        .len = sizeof(s_mouse_report_map),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = BLE_MOUSE_NAME,
    .manufacturer_name = "ikorg",
    .serial_number = "kvm-0001",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void post_ble_event(kvm_event_type_t type, kvm_host_t host)
{
    const kvm_event_t event = {
        .type = type,
        .host = host,
        .tick_ms = now_ms(),
    };
    if (g_kvm_event_queue != NULL) {
        xQueueSend(g_kvm_event_queue, &event, pdMS_TO_TICKS(10));
    }
}

static int find_slot_by_conn(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return SLOT_NONE;
    }
    for (int i = 0; i < 2; i++) {
        if (s_conn_handle[i] == conn_handle) {
            return i;
        }
    }
    return SLOT_NONE;
}

static int ble_mouse_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    struct ble_sm_io pkey = {0};
    int rc;

    if (event->type == BLE_GAP_EVENT_CONNECT) {
        s_advertising = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE connect failed status=%d", event->connect.status);
            update_advertising();
            return 0;
        }
        s_pending_conn_handle = event->connect.conn_handle;
        s_pending_conn_slot = s_advertising_for_slot;
        ESP_LOGI(TAG, "BLE connected conn=%u (target slot %s)",
                 s_pending_conn_handle,
                 s_pending_conn_slot != SLOT_NONE
                     ? kvm_host_name((kvm_host_t)s_pending_conn_slot)
                     : "?");

        struct ble_gap_upd_params conn_params = {
            .itvl_min = 6,  // 7.5 ms
            .itvl_max = 12, // 15 ms
            .latency = 0,
            .supervision_timeout = 400, // 4 s
            .min_ce_len = 0,
            .max_ce_len = 0,
        };
        rc = ble_gap_update_params(event->connect.conn_handle, &conn_params);
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE connection parameter update failed rc=%d", rc);
        }
        rc = ble_gap_security_initiate(event->connect.conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "BLE security initiate failed rc=%d", rc);
        }
        update_advertising();
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        const uint16_t conn = event->disconnect.conn.conn_handle;
        ESP_LOGW(TAG, "BLE GAP disconnected conn=%u reason=%d", conn,
                 event->disconnect.reason);

        const int slot = find_slot_by_conn(conn);
        kvm_host_t notify_slot = s_desired_slot;
        if (slot != SLOT_NONE) {
            s_conn_handle[slot] = BLE_HS_CONN_HANDLE_NONE;
            s_secure[slot] = false;
            if (s_hid_subscriber_conn[slot] == conn) {
                s_hid_subscriber_conn[slot] = BLE_HS_CONN_HANDLE_NONE;
            }
            notify_slot = (kvm_host_t)slot;
        } else if (s_pending_conn_handle == conn) {
            if (s_pending_conn_slot != SLOT_NONE) {
                notify_slot = (kvm_host_t)s_pending_conn_slot;
            }
            s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_pending_conn_slot = SLOT_NONE;
        }

        if (s_pending_pair_slot != SLOT_NONE) {
            s_pair_fail_count++;
            if (s_pair_fail_count >= PAIR_FAIL_BAILOUT) {
                ESP_LOGE(TAG,
                         "pairing for slot %s aborted after %d failed attempts; "
                         "disable Bluetooth on the other host and long-press again",
                         kvm_host_name((kvm_host_t)s_pending_pair_slot),
                         s_pair_fail_count);
                s_pending_pair_slot = SLOT_NONE;
                s_pair_fail_count = 0;
            }
        }

        post_ble_event(KVM_EVENT_BLE_HOST_DISCONNECTED, notify_slot);
        update_advertising();
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        s_advertising = false;
        ESP_LOGI(TAG, "BLE advertising complete reason=%d", event->adv_complete.reason);
        update_advertising();
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_ENC_CHANGE) {
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        ESP_LOGI(TAG,
                 "BLE encryption changed conn=%u status=%d encrypted=%d bonded=%d rc=%d",
                 event->enc_change.conn_handle,
                 event->enc_change.status,
                 rc == 0 ? desc.sec_state.encrypted : -1,
                 rc == 0 ? desc.sec_state.bonded : -1,
                 rc);
        if (event->enc_change.status != 0 || rc != 0 || !desc.sec_state.encrypted) {
            return 0;
        }

        kvm_host_t resolved;
        bool resolved_ok = false;

        if (s_pending_pair_slot != SLOT_NONE) {
            const kvm_host_t target = (kvm_host_t)s_pending_pair_slot;
            kvm_host_t existing;
            if (kvm_slots_lookup(&desc.peer_id_addr, &existing) && existing != target) {
                ESP_LOGW(TAG,
                         "peer already bound to slot %s; cancelling pairing intent for %s",
                         kvm_host_name(existing), kvm_host_name(target));
                s_pending_pair_slot = SLOT_NONE;
                resolved = existing;
                resolved_ok = true;
            } else {
                resolved = target;
                kvm_slots_set(resolved, &desc.peer_id_addr);
                s_pending_pair_slot = SLOT_NONE;
                resolved_ok = true;
                ESP_LOGI(TAG, "new bond stored for slot %s", kvm_host_name(resolved));
            }
        } else if (kvm_slots_lookup(&desc.peer_id_addr, &resolved)) {
            resolved_ok = true;
        }

        if (!resolved_ok) {
            ESP_LOGW(TAG, "encrypted peer not in any slot; dropping connection");
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        /* A single peer may open multiple BLE connections to us (e.g. macOS
         * CoreBluetooth creates a separate conn for a user-space app while
         * the system HID stack already holds one). Allow this — keep the
         * latest conn here for slot-level operations (terminate, etc.), but
         * mouse reports are routed via s_hid_subscriber_conn[]. */
        if (s_conn_handle[resolved] != BLE_HS_CONN_HANDLE_NONE &&
            s_conn_handle[resolved] != event->enc_change.conn_handle) {
            ESP_LOGI(TAG,
                     "slot %s already had conn %u; adding second conn %u",
                     kvm_host_name(resolved),
                     s_conn_handle[resolved],
                     event->enc_change.conn_handle);
        }
        s_conn_handle[resolved] = event->enc_change.conn_handle;
        s_secure[resolved] = true;
        if (s_pending_conn_handle == event->enc_change.conn_handle) {
            s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_pending_conn_slot = SLOT_NONE;
        }
        s_pair_fail_count = 0;
        kvm_slots_set_active(s_desired_slot);
        post_ble_event(KVM_EVENT_BLE_HOST_CONNECTED, resolved);
        update_advertising();
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_SUBSCRIBE) {
        ESP_LOGI(TAG,
                 "BLE subscribe conn=%u attr=%u notify=%d indicate=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        /* s_hid_report_attr is resolved up-front in ble_mouse_on_sync via
         * ble_gatts_find_chr; no observation-based capture here. */
        /* Remember which conn subscribed to the HID Report attribute. That
         * is the OS HID stack — separate from any companion conn on the
         * same peer that subscribes to other characteristics (warp_cmd). */
        if (event->subscribe.cur_notify && s_hid_report_attr != 0 &&
            event->subscribe.attr_handle == s_hid_report_attr) {
            const int slot = find_slot_by_conn(event->subscribe.conn_handle);
            if (slot != SLOT_NONE) {
                s_hid_subscriber_conn[slot] = event->subscribe.conn_handle;
                ESP_LOGI(TAG, "HID subscriber for slot %s = conn %u",
                         kvm_host_name((kvm_host_t)slot),
                         event->subscribe.conn_handle);
            }
        }
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_MTU) {
        ESP_LOGI(TAG, "BLE MTU updated to %d", event->mtu.value);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            kvm_host_t slot;
            if (kvm_slots_lookup(&desc.peer_id_addr, &slot)) {
                kvm_slots_clear(slot);
            }
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGW(TAG, "BLE repeat pairing; deleting old bond rc=%d", rc);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
        ESP_LOGI(TAG, "BLE passkey action=%d", event->passkey.params.action);
        pkey.action = event->passkey.params.action;

        if (event->passkey.params.action == BLE_SM_IOACT_DISP ||
            event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            pkey.passkey = 123456;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "BLE passkey injected rc=%d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.numcmp_accept = 1;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "BLE numeric comparison accepted rc=%d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            memset(pkey.oob, 0, sizeof(pkey.oob));
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "BLE OOB injected rc=%d", rc);
        }
        return 0;
    }
    return 0;
}

/* Continue advertising even when both slots are connected, but in
 * non-connectable mode. This lets a companion app on the active host's BT
 * stack discover us through a service-UUID-filtered scan, without occupying
 * a connection slot. The OS already owns a BLE connection to this peripheral
 * identity, so the companion's CoreBluetooth peripheral.connect() resolves to
 * that existing connection rather than opening a new one. */
static int start_broadcast_for(kvm_host_t slot)
{
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d (broadcast)", rc);
    }

    ble_addr_t identity;
    if (kvm_slots_ensure_identity(slot, &identity) != ESP_OK) {
        return -1;
    }
    rc = ble_hs_id_set_rnd(identity.val);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_hs_id_set_rnd rc=%d (broadcast)", rc);
    }
    ble_gap_wl_set(NULL, 0);

    /* For non-connectable advertising NimBLE uses ADV_NONCONN_IND which does
     * not solicit a scan response. Put the KVM service UUID in the adv data
     * itself (and skip the device name to keep the 31-byte budget). */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &BLE_KVM_CONTROL_SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "broadcast adv_set_fields rc=%d", rc);
        return rc;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.channel_map = BLE_ADV_CHANNEL_ALL;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(200);
    adv_params.filter_policy = BLE_HCI_ADV_FILT_NONE;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_mouse_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "broadcast adv_start rc=%d", rc);
        return rc;
    }

    s_advertising = true;
    s_pairing_mode = false;
    s_advertising_broadcast = true;
    s_advertising_for_slot = (int)slot;
    ESP_LOGI(TAG, "BROADCAST for slot %s identity", kvm_host_name(slot));
    return 0;
}

static int start_advertising_for(kvm_host_t slot, bool pairing)
{
    ble_addr_t bond_addr;
    const bool has_bond = kvm_slots_get(slot, &bond_addr);

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
    }

    ble_addr_t identity;
    if (kvm_slots_ensure_identity(slot, &identity) != ESP_OK) {
        ESP_LOGE(TAG, "failed to obtain identity for slot %s", kvm_host_name(slot));
        return -1;
    }
    rc = ble_hs_id_set_rnd(identity.val);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_hs_id_set_rnd rc=%d", rc);
    }

    if (!pairing && has_bond) {
        ble_gap_wl_set(&bond_addr, 1);
    } else {
        ble_gap_wl_set(NULL, 0);
    }

    struct ble_hs_adv_fields fields = {0};
    const char *name = BLE_MOUSE_NAME;
    const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(BLE_HID_SERVICE_UUID);
    const uint16_t appearance = ESP_HID_APPEARANCE_MOUSE;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.appearance = appearance;
    fields.appearance_is_present = 1;
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
        return rc;
    }

    /* Scan response carries the 128-bit KVM Control service UUID so that
     * companion apps can find this device using a service-UUID-filtered scan
     * (on macOS that also returns already-bonded peripherals). */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = &BLE_KVM_CONTROL_SVC_UUID;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.channel_map = BLE_ADV_CHANNEL_ALL;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(60);
    /* Filter only connect requests, not scan requests. This way only the
     * bonded peer of this slot can establish a link, but any scanner (e.g.
     * a companion app whose CoreBluetooth scan uses a different RPA) can
     * still receive the scan response with our service UUID. */
    adv_params.filter_policy = (!pairing && has_bond)
                                   ? BLE_HCI_ADV_FILT_CONN
                                   : BLE_HCI_ADV_FILT_NONE;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_mouse_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
        return rc;
    }

    s_advertising = true;
    s_pairing_mode = pairing;
    s_advertising_broadcast = false;
    s_advertising_for_slot = (int)slot;
    ESP_LOGI(TAG,
             "%s for slot %s: identity %02X:%02X:%02X:%02X:%02X:%02X",
             pairing ? "PAIRING" : "RECONNECT",
             kvm_host_name(slot),
             identity.val[5], identity.val[4], identity.val[3],
             identity.val[2], identity.val[1], identity.val[0]);
    return 0;
}

static void update_advertising(void)
{
    if (!s_started) {
        return;
    }

    int target_slot = SLOT_NONE;
    bool pairing = false;

    if (s_pending_pair_slot != SLOT_NONE &&
        s_conn_handle[s_pending_pair_slot] == BLE_HS_CONN_HANDLE_NONE) {
        target_slot = s_pending_pair_slot;
        pairing = true;
    } else {
        const kvm_host_t other_slot = s_desired_slot == KVM_HOST_LEFT
                                          ? KVM_HOST_RIGHT
                                          : KVM_HOST_LEFT;
        const kvm_host_t order[2] = {s_desired_slot, other_slot};
        for (int i = 0; i < 2; i++) {
            const kvm_host_t s = order[i];
            if (s_conn_handle[s] != BLE_HS_CONN_HANDLE_NONE) {
                continue;
            }
            ble_addr_t bond_addr;
            if (kvm_slots_get(s, &bond_addr)) {
                target_slot = (int)s;
                pairing = false;
                break;
            }
        }
    }

    if (target_slot == SLOT_NONE) {
        /* No connectable slot to advertise for. Run non-connectable broadcast
         * on the desired slot's identity so the companion app can discover
         * us through the OS BLE central. */
        if (s_advertising && s_advertising_broadcast &&
            s_advertising_for_slot == (int)s_desired_slot) {
            return;
        }
        start_broadcast_for(s_desired_slot);
        return;
    }

    if (s_advertising && !s_advertising_broadcast &&
        s_advertising_for_slot == target_slot &&
        s_pairing_mode == pairing) {
        return;
    }

    start_advertising_for((kvm_host_t)target_slot, pairing);
}

static void ble_mouse_on_sync(void)
{
    ble_svc_gap_device_name_set(BLE_MOUSE_NAME);
    ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_MOUSE);

    /* Resolve HID Input Report (UUID 0x2A4D) attribute handle directly from
     * the GATT DB. We can't rely on observing it via BLE_GAP_EVENT_SUBSCRIBE
     * because different OS HID stacks subscribe in different orders (macOS
     * goes HID-first; Windows often hits Battery Service first), and the
     * battery handle (~18) would be mistakenly captured as the mouse-report
     * attr, breaking HID delivery. */
    const ble_uuid_t *hid_svc = BLE_UUID16_DECLARE(BLE_HID_SERVICE_UUID);
    const ble_uuid_t *hid_report = BLE_UUID16_DECLARE(0x2A4D);
    uint16_t def_handle = 0, val_handle = 0;
    int rc = ble_gatts_find_chr(hid_svc, hid_report, &def_handle, &val_handle);
    if (rc == 0 && val_handle != 0) {
        s_hid_report_attr = val_handle;
        ESP_LOGI(TAG, "HID Input Report attr resolved from GATT DB: %u",
                 val_handle);
    } else {
        ESP_LOGW(TAG, "ble_gatts_find_chr(HID Input Report) rc=%d", rc);
    }
}

static void ble_mouse_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_mouse_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        s_started = true;
        ble_mouse_on_sync();
        ESP_LOGI(TAG, "BLE HID mouse started");
        update_advertising();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "BLE HID connected (awaiting encryption)");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGW(TAG, "BLE HID disconnected reason=%d",
                 param ? param->disconnect.reason : -1);
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG,
                 "BLE HID protocol mode map=%u mode=%s",
                 param->protocol_mode.map_index,
                 param->protocol_mode.protocol_mode == ESP_HID_PROTOCOL_MODE_REPORT ? "REPORT" : "BOOT");
        break;
    case ESP_HIDD_CONTROL_EVENT:
        ESP_LOGI(TAG, "BLE HID control=%u", param->control.control);
        break;
    default:
        break;
    }
}

esp_err_t ble_mouse_start(void)
{
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    kvm_slots_prune_orphan_bonds();

    kvm_host_t persisted_slot;
    if (kvm_slots_get_active(&persisted_slot)) {
        s_desired_slot = persisted_slot;
        ESP_LOGI(TAG, "resuming active slot %s from NVS", kvm_host_name(persisted_slot));
    }

    ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, ble_mouse_hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ble_control_register();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_control_register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_nimble_enable(ble_mouse_host_task);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BLE mouse ready; long-press to enter pairing mode");
    return ESP_OK;
}

esp_err_t ble_mouse_select_host(kvm_host_t host)
{
    s_desired_slot = host;

    if (s_pending_pair_slot != SLOT_NONE && s_pending_pair_slot != (int)host) {
        ESP_LOGI(TAG,
                 "cancelling pending pairing for %s due to switch to %s",
                 kvm_host_name((kvm_host_t)s_pending_pair_slot),
                 kvm_host_name(host));
        s_pending_pair_slot = SLOT_NONE;
        s_pair_fail_count = 0;
    }

    if (s_conn_handle[host] != BLE_HS_CONN_HANDLE_NONE && s_secure[host]) {
        kvm_slots_set_active(host);
        update_advertising();
        return ESP_OK;
    }

    update_advertising();
    ble_addr_t tmp;
    return kvm_slots_get(host, &tmp) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t ble_mouse_start_pairing(kvm_host_t host)
{
    ble_addr_t old;
    if (kvm_slots_get(host, &old)) {
        ble_store_util_delete_peer(&old);
        ESP_LOGI(TAG, "removed previous bond for slot %s", kvm_host_name(host));
    }
    kvm_slots_clear(host);

    s_desired_slot = host;
    s_pending_pair_slot = (int)host;
    s_pair_fail_count = 0;

    if (s_conn_handle[host] != BLE_HS_CONN_HANDLE_NONE) {
        const int rc = ble_gap_terminate(s_conn_handle[host],
                                          BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
        }
        return ESP_OK;
    }

    update_advertising();
    return ESP_OK;
}

esp_err_t ble_mouse_send_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (s_hid_report_attr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Send to the conn that subscribed to the HID Input Report, not the
     * generic slot conn — they may differ when a companion app holds a
     * second BLE link on the same peer. */
    const uint16_t conn = s_hid_subscriber_conn[s_desired_slot];
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_secure[s_desired_slot]) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[4] = {
        buttons & 0x07,
        (uint8_t)dx,
        (uint8_t)dy,
        (uint8_t)wheel,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const int rc = ble_gatts_notify_custom(conn, s_hid_report_attr, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool ble_mouse_is_connected(void)
{
    return s_hid_subscriber_conn[s_desired_slot] != BLE_HS_CONN_HANDLE_NONE &&
           s_secure[s_desired_slot];
}

bool ble_mouse_is_advertising(void)
{
    return s_advertising;
}

bool ble_mouse_is_pairing(void)
{
    return s_advertising && s_pairing_mode;
}

bool ble_mouse_slot_for_conn(uint16_t conn_handle, kvm_host_t *out_slot)
{
    const int slot = find_slot_by_conn(conn_handle);
    if (slot == SLOT_NONE) {
        return false;
    }
    *out_slot = (kvm_host_t)slot;
    return true;
}

uint16_t ble_mouse_conn_for_slot(kvm_host_t slot)
{
    if (!s_secure[slot]) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    return s_conn_handle[slot];
}

void ble_mouse_force_select(kvm_host_t slot)
{
    /* Push the same event the manual button path uses; kvm_state_handle_event
     * will then update active_host, LEDs, buzzer, NVS-persisted active slot,
     * and call back into ble_mouse_select_host to update advertising. */
    const kvm_event_type_t ev = (slot == KVM_HOST_LEFT)
                                    ? KVM_EVENT_GATT_SWITCH_TO_LEFT
                                    : KVM_EVENT_GATT_SWITCH_TO_RIGHT;
    post_ble_event(ev, slot);
}
