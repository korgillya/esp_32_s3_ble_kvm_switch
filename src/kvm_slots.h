/**
 * @file kvm_slots.h
 * @brief NVS-backed mapping between KVM slots and BLE peer/local addresses.
 *
 * Each slot (LEFT / RIGHT) is identified by:
 *  - The peer identity address of the laptop it is paired with.
 *  - A local random static identity address ESP advertises under for that
 *    slot, so each laptop sees ESP as its own peripheral.
 *
 * This module also tracks the *last active slot* across reboots so the
 * firmware boots straight back into the host the user was on.
 *
 * The data lives in NVS namespace `kvm_slots`. It is separate from NimBLE's
 * own bond store (which keys bonds by peer address only) — `kvm_slots` adds
 * the slot dimension on top.
 */

#pragma once

#include <stdbool.h>

#include "app_events.h"
#include "esp_err.h"
#include "nimble/ble.h"

/**
 * @brief Initialise NVS (idempotent — safe to call multiple times).
 * @return Result of `nvs_flash_init`; on `ESP_ERR_NVS_NO_FREE_PAGES` or
 *         `ESP_ERR_NVS_NEW_VERSION_FOUND` the partition is erased and
 *         re-initialised automatically.
 */
esp_err_t kvm_slots_init(void);

/**
 * @brief Read the bonded peer address for a slot.
 * @param host  Slot index.
 * @param[out] out Address written on success.
 * @return true if the slot has a stored bond, false if empty.
 */
bool kvm_slots_get(kvm_host_t host, ble_addr_t *out);

/**
 * @brief Bind a peer address to a slot.
 *
 * If the same address was already stored in the *other* slot it is cleared
 * there first — a peer can belong to at most one slot at a time.
 *
 * @param host  Target slot.
 * @param addr  Peer identity address to store.
 * @return `nvs_*` result.
 */
esp_err_t kvm_slots_set(kvm_host_t host, const ble_addr_t *addr);

/**
 * @brief Forget the bonded peer for a slot.
 *
 * Does not touch the NimBLE bond store; the caller is responsible for
 * `ble_store_util_delete_peer` if needed.
 */
esp_err_t kvm_slots_clear(kvm_host_t host);

/**
 * @brief Find which slot owns a given peer address.
 * @param addr  Peer identity address.
 * @param[out] out_host Set to LEFT/RIGHT on hit.
 * @return true if the address is bound to a slot, false otherwise.
 */
bool kvm_slots_lookup(const ble_addr_t *addr, kvm_host_t *out_host);

/**
 * @brief Read the persisted "last active slot".
 * @param[out] out Active slot on success.
 * @return true if the value is present (i.e. NVS was written at least once).
 */
bool kvm_slots_get_active(kvm_host_t *out);

/**
 * @brief Persist the currently active slot so the next boot resumes there.
 */
esp_err_t kvm_slots_set_active(kvm_host_t host);

/**
 * @brief Get (or generate-and-store) the local BLE identity address for a
 *        slot.
 *
 * On the first call for a given slot, generates a fresh random static
 * address via `ble_hs_id_gen_rnd(0, ...)` and persists it. Subsequent calls
 * return the same address. Identities never change for the life of an NVS
 * partition; this is what keeps a paired host from auto-connecting to the
 * wrong slot.
 *
 * @param host  Slot index.
 * @param[out] out Identity address.
 * @return ESP_OK on success.
 */
esp_err_t kvm_slots_ensure_identity(kvm_host_t host, ble_addr_t *out);

/**
 * @brief Remove NimBLE bonds whose peer is not present in `kvm_slots`.
 *
 * Called once at startup. Defensive cleanup for cases where a firmware
 * update changed slot mapping logic but left the NimBLE bond store intact;
 * stranded bonds would otherwise let a forgotten peer auto-reconnect.
 */
void kvm_slots_prune_orphan_bonds(void);
