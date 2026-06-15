//! BLE client for the KVM Control GATT service via `bluest`.
//!
//! `bluest` is used (instead of `btleplug`) because on macOS it exposes
//! `Adapter::connected_devices_with_services`, which wraps Apple's
//! `retrieveConnectedPeripherals(withServices:)`. That's the only API path
//! that returns peripherals already bonded and connected via the system HID
//! stack — a plain `scanForPeripherals` callback filters them out for
//! user-space apps.
//!
//! UUIDs and wire format must match `docs/gatt-contract.md`. The macOS
//! quirks (handle refresh, `Box::leak` on characteristics, single-thread
//! runtime, keepalive reconnect) are documented in `../../AGENTS.md` —
//! don't strip them without re-testing on macOS.

use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, bail, Result};
use bluest::{Adapter, Characteristic, Device, Service, Uuid};
use futures::stream::Stream;
use futures_util::StreamExt;
use tokio::time::sleep;
use tracing::{debug, info, warn};

use crate::cursor;

/// KVM Control service UUID (`6b766d63-6f6e-7472-6f6c-000000000001`).
pub const SVC_UUID: Uuid = Uuid::from_u128(0x6b766d63_6f6e_7472_6f6c_000000000001);
/// Characteristic UUID for `slot_id` (read-only, 1 byte).
pub const CHR_SLOT_ID: Uuid = Uuid::from_u128(0x6b766d63_6f6e_7472_6f6c_000000000002);
/// Characteristic UUID for `edge_event` (write, 3 bytes).
pub const CHR_EDGE_EVENT: Uuid = Uuid::from_u128(0x6b766d63_6f6e_7472_6f6c_000000000003);
/// Characteristic UUID for `warp_cmd` (notify, 3 bytes).
pub const CHR_WARP_CMD: Uuid = Uuid::from_u128(0x6b766d63_6f6e_7472_6f6c_000000000004);

/// BLE HID service (0x1812) expanded into the Bluetooth base 128-bit UUID.
///
/// Used to ask macOS "which peripherals are HID-connected?" — those already
/// have their HID services cached by the OS, so the call succeeds even when
/// our custom KVM Control service hasn't yet been discovered.
pub const HID_SVC_UUID: Uuid = Uuid::from_u128(0x0000_1812_0000_1000_8000_00805f9b34fb);

/// Which KVM slot this companion instance represents.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Slot {
    Left = 0,
    Right = 1,
}

impl Slot {
    /// Decode the byte returned by the firmware's `slot_id` characteristic.
    pub fn from_byte(b: u8) -> Result<Self> {
        match b {
            0 => Ok(Slot::Left),
            1 => Ok(Slot::Right),
            other => bail!("unknown slot id {}", other),
        }
    }
}

/// Outbound edge crossing code, matches the firmware's `EDGE_CODE_*` macros.
#[derive(Clone, Copy, Debug)]
#[allow(dead_code)] // Top/Bottom reserved for vertical layouts (v2)
pub enum EdgeCode {
    Right = 1,
    Left = 2,
    Top = 3,
    Bottom = 4,
}

/// Inbound entry-side code carried by a `warp_cmd` notification.
#[derive(Clone, Copy, Debug)]
pub enum EntrySide {
    FromLeft = 1,
    FromRight = 2,
    FromTop = 3,
    FromBottom = 4,
}

impl EntrySide {
    fn from_byte(b: u8) -> Option<Self> {
        match b {
            1 => Some(Self::FromLeft),
            2 => Some(Self::FromRight),
            3 => Some(Self::FromTop),
            4 => Some(Self::FromBottom),
            _ => None,
        }
    }
}

/// Live GATT session against the ESP peripheral.
///
/// Returned from [`connect`] together with a [`WarpStream`] of inbound
/// notifications. The fields below need to stay alive for as long as
/// the daemon wants to issue GATT operations.
#[derive(Clone)]
pub struct Session {
    /// Held to keep the connection alive while we use its characteristics.
    #[allow(dead_code)]
    pub device: Arc<Device>,
    /// Held alive so that `Characteristic` handles remain valid for
    /// notify/write. Dropping the parent `Service` invalidates its
    /// characteristics in bluest's internal cache.
    #[allow(dead_code)]
    pub service: Arc<Service>,
    /// Which laptop this companion represents (read from the firmware once
    /// per startup).
    pub slot: Slot,
    /// Leaked into `'static` to avoid bluest's "device isn't connected" error
    /// path on subsequent writes — see the module-level doc comment.
    pub edge_event_chr: &'static Characteristic,
}

/// Pinned, boxed stream of inbound `warp_cmd` notifications.
pub type WarpStream = Pin<Box<dyn Stream<Item = std::result::Result<Vec<u8>, bluest::Error>> + Send>>;

/// Scan for the ESP peripheral, connect to it, resolve our custom KVM
/// Control characteristics, subscribe to `warp_cmd`, and return everything
/// the main loop needs.
///
/// The returned [`Adapter`] is used by [`ensure_connected`] for keepalive
/// reconnects.
pub async fn connect(device_name: &str) -> Result<(Session, WarpStream, Adapter)> {
    let adapter = Adapter::default()
        .await
        .ok_or_else(|| anyhow!("no Bluetooth adapter available"))?;
    adapter.wait_available().await?;

    let mut device = find_device(&adapter, device_name).await?;
    info!(name = ?device.name_async().await.ok(), "connecting to peripheral");

    adapter.connect_device(&device).await?;

    /* On bluest macOS the Device returned from a scan does not get its
     * internal state updated to "connected" by Adapter::connect_device.
     * We try to refresh through two paths and fall back to the original
     * handle. */
    tokio::time::sleep(Duration::from_millis(150)).await;
    let connected_before = device.is_connected().await;
    debug!(connected_before, "device state before refresh");
    if let Ok(fresh) = adapter.open_device(&device.id()).await {
        debug!("refreshed device handle via Adapter::open_device");
        device = fresh;
    } else if let Ok(connected) = adapter.connected_devices().await {
        if let Some(fresh) = connected.into_iter().find(|d| d.id() == device.id()) {
            debug!("refreshed device handle via connected_devices");
            device = fresh;
        }
    }
    let connected_after = device.is_connected().await;
    debug!(connected_after, "device state after refresh");

    /* Force a full GATT discovery so macOS caches every service the device
     * exposes (including ours). Some Core Bluetooth code paths skip
     * already-cached services if you query by UUID without a prior full
     * discovery. */
    let all_services = device.discover_services().await?;
    debug!(count = all_services.len(), "discovered services");
    let service = all_services
        .into_iter()
        .find(|s| s.uuid() == SVC_UUID)
        .ok_or_else(|| anyhow!("KVM control service not present on peripheral"))?;

    let characteristics = service.characteristics().await?;

    let slot_chr = find_characteristic(&characteristics, CHR_SLOT_ID, "slot_id")?;
    let edge_chr = find_characteristic(&characteristics, CHR_EDGE_EVENT, "edge_event")?;
    let warp_chr = find_characteristic(&characteristics, CHR_WARP_CMD, "warp_cmd")?;

    let slot_bytes = slot_chr.read().await?;
    if slot_bytes.is_empty() {
        bail!("slot_id read returned empty payload");
    }
    let slot = Slot::from_byte(slot_bytes[0])?;
    info!(?slot, "resolved slot");

    /* Subscribe to warp_cmd notifications in the same async context as the
     * rest of the GATT setup. Doing this from a tokio::spawn'd task later
     * makes bluest report the device as disconnected (CoreBluetooth thread
     * affinity quirk). */
    /* bluest's notify() returns a stream that borrows the characteristic.
     * To carry that stream out of this function (and into a spawn'd task),
     * we leak a 'static clone of the characteristic. The leak happens once
     * per process startup and is on the order of bytes. */
    let warp_chr_static: &'static Characteristic = Box::leak(Box::new(warp_chr.clone()));
    let warp_stream: WarpStream = {
        let mut attempt = 0;
        loop {
            attempt += 1;
            let connected = device.is_connected().await;
            debug!(attempt, connected, "subscribing to warp_cmd");
            match warp_chr_static.notify().await {
                Ok(s) => break Box::pin(s) as WarpStream,
                Err(e) if attempt < 10 => {
                    warn!(error = ?e, attempt, "warp subscribe retry");
                    sleep(Duration::from_millis(300)).await;
                }
                Err(e) => return Err(e.into()),
            }
        }
    };
    info!("subscribed to warp_cmd notifications");

    let edge_chr_static: &'static Characteristic = Box::leak(Box::new(edge_chr));
    let session = Session {
        device: Arc::new(device),
        service: Arc::new(service),
        slot,
        edge_event_chr: edge_chr_static,
    };
    Ok((session, warp_stream, adapter))
}

/// Periodic keepalive called from the main loop.
///
/// Re-issues `connect_device` if bluest reports the device as disconnected,
/// so that subsequent GATT operations have a live link. On macOS this also
/// keeps CoreBluetooth from putting the link into a power-saving state that
/// would fail `write_without_response`.
pub async fn ensure_connected(adapter: &Adapter, session: &Session) {
    let is_connected = session.device.is_connected().await;
    if !is_connected {
        warn!("device reports not connected; reconnecting");
        if let Err(e) = adapter.connect_device(&session.device).await {
            warn!(error = ?e, "reconnect failed");
        }
    }
}

async fn find_device(adapter: &Adapter, device_name: &str) -> Result<Device> {
    let timeout = Duration::from_secs(20);
    let deadline = std::time::Instant::now() + timeout;

    /* Try several discovery paths in a loop. The peripheral may flicker
     * between connected/disconnected as the OS HID stack reconnects, so we
     * give multiple paths repeated chances. */
    let mut attempt = 0;
    while std::time::Instant::now() < deadline {
        attempt += 1;

        // Path 1: peripherals OS-connected and exposing the HID service.
        match adapter.connected_devices_with_services(&[HID_SVC_UUID]).await {
            Ok(devs) => {
                debug!(attempt, count = devs.len(),
                       "connected_devices_with_services(HID)");
                for d in devs {
                    let name = d.name_async().await.ok();
                    debug!(name = ?name, id = ?d.id(), "candidate (HID-connected)");
                    if name.as_deref() == Some(device_name) {
                        info!("found peripheral via HID connected_devices");
                        return Ok(d);
                    }
                }
            }
            Err(e) => warn!(error = ?e, "connected_devices_with_services(HID) failed"),
        }

        // Path 2: any OS-connected peripheral (no service filter).
        match adapter.connected_devices().await {
            Ok(devs) => {
                debug!(attempt, count = devs.len(), "connected_devices");
                for d in devs {
                    let name = d.name_async().await.ok();
                    debug!(name = ?name, id = ?d.id(), "candidate (any-connected)");
                    if name.as_deref() == Some(device_name) {
                        info!("found peripheral via connected_devices");
                        return Ok(d);
                    }
                }
            }
            Err(e) => warn!(error = ?e, "connected_devices failed"),
        }

        // Path 3: brief scan window for advertising peripherals.
        match adapter.scan(&[]).await {
            Ok(mut scan) => {
                let scan_deadline = std::time::Instant::now() + Duration::from_secs(2);
                while std::time::Instant::now() < scan_deadline {
                    match tokio::time::timeout(Duration::from_millis(500), scan.next())
                        .await
                    {
                        Ok(Some(adv)) => {
                            let name = adv.adv_data.local_name.clone();
                            debug!(
                                id = ?adv.device.id(),
                                name = ?name,
                                services = ?adv.adv_data.services,
                                "scan hit"
                            );
                            let name_matches = name.as_deref() == Some(device_name);
                            let service_matches = adv.adv_data.services.contains(&SVC_UUID);
                            if name_matches || service_matches {
                                info!("found peripheral via scan");
                                return Ok(adv.device);
                            }
                        }
                        Ok(None) => break,
                        Err(_) => {} // timeout, keep polling
                    }
                }
            }
            Err(e) => warn!(error = ?e, "scan failed"),
        }

        sleep(Duration::from_millis(500)).await;
    }
    bail!(
        "could not find '{}' as connected nor advertising peripheral after {:?}",
        device_name,
        timeout
    );
}

fn find_characteristic(
    characteristics: &[Characteristic],
    uuid: Uuid,
    label: &str,
) -> Result<Characteristic> {
    characteristics
        .iter()
        .find(|c| c.uuid() == uuid)
        .cloned()
        .ok_or_else(|| anyhow!("{} characteristic missing on peripheral", label))
}

/// Push one edge crossing event to the firmware.
///
/// Writes 3 bytes (`edge_code`, `pos_norm_lo`, `pos_norm_hi`) to the
/// `edge_event` characteristic. The firmware reacts by switching its active
/// slot to the neighbouring host (if any) and notifying that host's
/// companion via [`WarpStream`].
pub async fn send_edge_event(
    session: &Session,
    code: EdgeCode,
    pos_norm: u16,
) -> Result<()> {
    let payload = [
        code as u8,
        (pos_norm & 0xFF) as u8,
        ((pos_norm >> 8) & 0xFF) as u8,
    ];
    session
        .edge_event_chr
        .write_without_response(&payload)
        .await?;
    debug!(code = ?code, pos_norm, "edge_event sent");
    Ok(())
}

/// Apply one warp_cmd notification value (received from the warp stream
/// inside the main select loop).
pub fn handle_warp_value(value: &[u8]) {
    if value.len() < 3 {
        warn!(len = value.len(), "warp_cmd notification too short");
        return;
    }
    let entry = match EntrySide::from_byte(value[0]) {
        Some(e) => e,
        None => {
            warn!(byte = value[0], "unknown entry_side");
            return;
        }
    };
    let pos_norm = (value[1] as u16) | ((value[2] as u16) << 8);
    info!(?entry, pos_norm, "warp_cmd received");
    if let Err(e) = cursor::warp(entry, pos_norm) {
        warn!(error = ?e, "warp failed");
    }
}
