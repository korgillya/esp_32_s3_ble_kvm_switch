#include "kvm_slots.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "kvm_state.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kvm-slots";
static const char *NS = "kvm_slots";
static const char *KEY_ACTIVE = "act";

static const char *slot_key(kvm_host_t host)
{
    return host == KVM_HOST_LEFT ? "L" : "R";
}

static const char *id_key(kvm_host_t host)
{
    return host == KVM_HOST_LEFT ? "iL" : "iR";
}

static bool addr_equal(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

esp_err_t kvm_slots_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool kvm_slots_get(kvm_host_t host, ble_addr_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    const esp_err_t err = nvs_get_blob(h, slot_key(host), out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*out);
}

esp_err_t kvm_slots_set(kvm_host_t host, const ble_addr_t *addr)
{
    const kvm_host_t other = host == KVM_HOST_LEFT ? KVM_HOST_RIGHT : KVM_HOST_LEFT;
    ble_addr_t other_addr;
    if (kvm_slots_get(other, &other_addr) && addr_equal(&other_addr, addr)) {
        ESP_LOGW(TAG,
                 "peer already bound to slot %s; clearing it before binding to %s",
                 kvm_host_name(other),
                 kvm_host_name(host));
        kvm_slots_clear(other);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, slot_key(host), addr, sizeof(*addr));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "slot %s bound to %02X:%02X:%02X:%02X:%02X:%02X type=%u",
                 kvm_host_name(host),
                 addr->val[5], addr->val[4], addr->val[3],
                 addr->val[2], addr->val[1], addr->val[0],
                 addr->type);
    }
    return err;
}

bool kvm_slots_get_active(kvm_host_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    const esp_err_t err = nvs_get_u8(h, KEY_ACTIVE, &value);
    nvs_close(h);
    if (err != ESP_OK || value > KVM_HOST_RIGHT) {
        return false;
    }
    *out = (kvm_host_t)value;
    return true;
}

esp_err_t kvm_slots_set_active(kvm_host_t host)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_ACTIVE, (uint8_t)host);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t kvm_slots_ensure_identity(kvm_host_t host, ble_addr_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*out);
    err = nvs_get_blob(h, id_key(host), out, &len);
    if (err == ESP_OK && len == sizeof(*out)) {
        nvs_close(h);
        return ESP_OK;
    }

    nvs_close(h);

    const int rc = ble_hs_id_gen_rnd(0, out);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_gen_rnd rc=%d", rc);
        return ESP_FAIL;
    }

    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, id_key(host), out, sizeof(*out));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG,
             "generated identity for slot %s: %02X:%02X:%02X:%02X:%02X:%02X",
             kvm_host_name(host),
             out->val[5], out->val[4], out->val[3],
             out->val[2], out->val[1], out->val[0]);
    return err;
}

void kvm_slots_prune_orphan_bonds(void)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    int rc = ble_store_util_bonded_peers(peers, &count,
                                         sizeof(peers) / sizeof(peers[0]));
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_util_bonded_peers rc=%d", rc);
        return;
    }
    int pruned = 0;
    for (int i = 0; i < count; i++) {
        kvm_host_t slot;
        if (!kvm_slots_lookup(&peers[i], &slot)) {
            ESP_LOGW(TAG,
                     "pruning orphan bond %02X:%02X:%02X:%02X:%02X:%02X (type=%u)",
                     peers[i].val[5], peers[i].val[4], peers[i].val[3],
                     peers[i].val[2], peers[i].val[1], peers[i].val[0],
                     peers[i].type);
            ble_store_util_delete_peer(&peers[i]);
            pruned++;
        }
    }
    ESP_LOGI(TAG, "bond store pruning: kept=%d pruned=%d", count - pruned, pruned);
}

esp_err_t kvm_slots_clear(kvm_host_t host)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, slot_key(host));
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "slot %s cleared", kvm_host_name(host));
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

bool kvm_slots_lookup(const ble_addr_t *addr, kvm_host_t *out_host)
{
    ble_addr_t bond;
    if (kvm_slots_get(KVM_HOST_LEFT, &bond) &&
        bond.type == addr->type &&
        memcmp(bond.val, addr->val, sizeof(bond.val)) == 0) {
        *out_host = KVM_HOST_LEFT;
        return true;
    }
    if (kvm_slots_get(KVM_HOST_RIGHT, &bond) &&
        bond.type == addr->type &&
        memcmp(bond.val, addr->val, sizeof(bond.val)) == 0) {
        *out_host = KVM_HOST_RIGHT;
        return true;
    }
    return false;
}
