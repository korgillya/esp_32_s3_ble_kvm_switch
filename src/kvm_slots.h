#pragma once

#include <stdbool.h>

#include "app_events.h"
#include "esp_err.h"
#include "nimble/ble.h"

esp_err_t kvm_slots_init(void);
bool kvm_slots_get(kvm_host_t host, ble_addr_t *out);
esp_err_t kvm_slots_set(kvm_host_t host, const ble_addr_t *addr);
esp_err_t kvm_slots_clear(kvm_host_t host);
bool kvm_slots_lookup(const ble_addr_t *addr, kvm_host_t *out_host);

bool kvm_slots_get_active(kvm_host_t *out);
esp_err_t kvm_slots_set_active(kvm_host_t host);

esp_err_t kvm_slots_ensure_identity(kvm_host_t host, ble_addr_t *out);

void kvm_slots_prune_orphan_bonds(void);
