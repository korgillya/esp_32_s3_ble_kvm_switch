#include "ble_control.h"

#include <string.h>

#include "app_events.h"
#include "ble_mouse.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "kvm_state.h"
#include "nimble/ble.h"

static const char *TAG = "ble-control";

/* See docs/gatt-contract.md. Display UUID: 6b766d63-6f6e-7472-6f6c-000000000XXX
 * NimBLE BLE_UUID128_INIT stores bytes LSB-first; the bytes below decode to
 * the desired display order. */
const ble_uuid128_t BLE_KVM_CONTROL_SVC_UUID = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6f,
    0x72, 0x74, 0x6e, 0x6f, 0x63, 0x6d, 0x76, 0x6b);

static const ble_uuid128_t CHR_SLOT_ID_UUID = BLE_UUID128_INIT(
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6f,
    0x72, 0x74, 0x6e, 0x6f, 0x63, 0x6d, 0x76, 0x6b);

static const ble_uuid128_t CHR_EDGE_EVENT_UUID = BLE_UUID128_INIT(
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6f,
    0x72, 0x74, 0x6e, 0x6f, 0x63, 0x6d, 0x76, 0x6b);

static const ble_uuid128_t CHR_WARP_CMD_UUID = BLE_UUID128_INIT(
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6f,
    0x72, 0x74, 0x6e, 0x6f, 0x63, 0x6d, 0x76, 0x6b);

#define EDGE_CODE_RIGHT 1
#define EDGE_CODE_LEFT 2
#define EDGE_CODE_TOP 3
#define EDGE_CODE_BOTTOM 4

#define ENTRY_SIDE_FROM_LEFT 1
#define ENTRY_SIDE_FROM_RIGHT 2

static uint16_t s_warp_attr_handle;

static int slot_id_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    kvm_host_t slot;
    if (!ble_mouse_slot_for_conn(conn_handle, &slot)) {
        ESP_LOGW(TAG, "slot_id read from unresolved conn=%u", conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    const uint8_t value = (uint8_t)slot;
    const int rc = os_mbuf_append(ctxt->om, &value, sizeof(value));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int edge_event_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    if (OS_MBUF_PKTLEN(ctxt->om) < 3) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t payload[3];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &copied);
    if (rc != 0 || copied != sizeof(payload)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    kvm_host_t source_slot;
    if (!ble_mouse_slot_for_conn(conn_handle, &source_slot)) {
        ESP_LOGW(TAG, "edge_event from unresolved conn=%u", conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    const uint8_t edge_code = payload[0];
    const uint16_t pos_norm = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);

    if (edge_code != EDGE_CODE_LEFT && edge_code != EDGE_CODE_RIGHT) {
        ESP_LOGI(TAG, "ignoring edge_code=%u from slot %s (only left/right used)",
                 edge_code, kvm_host_name(source_slot));
        return 0;
    }

    /* v1 layout: slot LEFT is left monitor, slot RIGHT is right monitor.
     * Right-edge of LEFT  -> switch to RIGHT, enter from left.
     * Left-edge of RIGHT  -> switch to LEFT,  enter from right.
     * Other combinations (left-edge of LEFT, right-edge of RIGHT) are no-ops
     * because there is no neighbor in that direction. */
    kvm_host_t target_slot;
    uint8_t entry_side;
    if (source_slot == KVM_HOST_LEFT && edge_code == EDGE_CODE_RIGHT) {
        target_slot = KVM_HOST_RIGHT;
        entry_side = ENTRY_SIDE_FROM_LEFT;
    } else if (source_slot == KVM_HOST_RIGHT && edge_code == EDGE_CODE_LEFT) {
        target_slot = KVM_HOST_LEFT;
        entry_side = ENTRY_SIDE_FROM_RIGHT;
    } else {
        ESP_LOGI(TAG,
                 "edge_code=%u from slot %s has no neighbor in that direction",
                 edge_code, kvm_host_name(source_slot));
        return 0;
    }

    const uint16_t target_conn = ble_mouse_conn_for_slot(target_slot);
    if (target_conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG,
                 "edge_event %s->%s but target slot is not connected; "
                 "switching desired slot anyway",
                 kvm_host_name(source_slot), kvm_host_name(target_slot));
        ble_mouse_force_select(target_slot);
        return 0;
    }

    ESP_LOGI(TAG,
             "edge %s of slot %s -> switch to %s, entry side=%u pos=%u",
             edge_code == EDGE_CODE_RIGHT ? "RIGHT" : "LEFT",
             kvm_host_name(source_slot),
             kvm_host_name(target_slot),
             entry_side,
             pos_norm);

    ble_mouse_force_select(target_slot);

    if (s_warp_attr_handle != 0) {
        uint8_t warp_payload[3] = {
            entry_side,
            (uint8_t)(pos_norm & 0xFF),
            (uint8_t)((pos_norm >> 8) & 0xFF),
        };
        struct os_mbuf *om = ble_hs_mbuf_from_flat(warp_payload,
                                                   sizeof(warp_payload));
        if (om == NULL) {
            ESP_LOGW(TAG, "warp_cmd mbuf alloc failed");
            return 0;
        }
        const int notify_rc = ble_gatts_notify_custom(target_conn,
                                                      s_warp_attr_handle, om);
        if (notify_rc != 0) {
            ESP_LOGW(TAG, "warp_cmd notify rc=%d", notify_rc);
        }
    }
    return 0;
}

static int warp_cmd_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* Notify-only characteristic; reads return empty. */
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &CHR_SLOT_ID_UUID.u,
        .access_cb = slot_id_access,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &CHR_EDGE_EVENT_UUID.u,
        .access_cb = edge_event_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &CHR_WARP_CMD_UUID.u,
        .access_cb = warp_cmd_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
        .val_handle = &s_warp_attr_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &BLE_KVM_CONTROL_SVC_UUID.u,
        .characteristics = s_chrs,
    },
    { 0 }
};

esp_err_t ble_control_register(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "KVM control service registered");
    return ESP_OK;
}
