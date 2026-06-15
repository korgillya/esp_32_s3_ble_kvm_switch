//! Cursor edge detection.
//!
//! Polls the OS cursor position at the rate chosen on the command line and
//! fires an `edge_event` GATT write whenever the cursor enters the 1-pixel
//! band at the left or right side of the primary screen. Vertical edges are
//! ignored in v1 (the firmware also ignores them).
//!
//! A simple "armed" flag per edge prevents spamming: once an event is sent,
//! the cursor must travel at least [`REARM_PX`] away from that edge before
//! another event will be emitted for it.

use anyhow::Result;
use tracing::{debug, warn};

use crate::ble::{self, EdgeCode, Session};
use crate::cursor::{self, ScreenBounds};

/// How many pixels from the edge counts as "the cursor hit the edge".
const EDGE_BAND_PX: f64 = 1.0;
/// Re-arm distance: cursor must move at least this many pixels away from the
/// edge before another event for that edge is fired.
const REARM_PX: f64 = 8.0;

/// Per-edge "armed" flags. An edge is rearmed once the cursor moves away.
#[derive(Default)]
pub struct EdgeArming {
    pub right_armed: bool,
    pub left_armed: bool,
}

impl EdgeArming {
    /// Initial state: both edges ready to fire.
    pub fn ready() -> Self {
        Self {
            right_armed: true,
            left_armed: true,
        }
    }
}

/// Single poll iteration.
///
/// Must be called from the **main** tokio task: bluest's CoreBluetooth
/// backend treats the device as disconnected if GATT operations are invoked
/// from a different tokio worker than the one that created the connection.
/// In practice this means the companion runs everything in one
/// `tokio::select!` loop instead of spawning per-stream tasks.
pub async fn poll_once(
    session: &Session,
    bounds: ScreenBounds,
    arming: &mut EdgeArming,
) -> Result<()> {
    let pos = match cursor::current_position() {
        Ok(p) => p,
        Err(e) => {
            warn!(error = ?e, "cursor read failed");
            return Ok(());
        }
    };

    // Right edge.
    if pos.x >= bounds.width - EDGE_BAND_PX {
        if arming.right_armed {
            let y_norm = clamp_norm(pos.y / bounds.height);
            debug!(x = pos.x, y = pos.y, y_norm, "right edge hit");
            if let Err(e) = ble::send_edge_event(session, EdgeCode::Right, y_norm).await {
                warn!(error = ?e, "send_edge_event right failed");
            } else {
                arming.right_armed = false;
            }
        }
    } else if pos.x < bounds.width - EDGE_BAND_PX - REARM_PX {
        arming.right_armed = true;
    }

    // Left edge.
    if pos.x <= EDGE_BAND_PX - 1.0 {
        if arming.left_armed {
            let y_norm = clamp_norm(pos.y / bounds.height);
            debug!(x = pos.x, y = pos.y, y_norm, "left edge hit");
            if let Err(e) = ble::send_edge_event(session, EdgeCode::Left, y_norm).await {
                warn!(error = ?e, "send_edge_event left failed");
            } else {
                arming.left_armed = false;
            }
        }
    } else if pos.x > EDGE_BAND_PX + REARM_PX {
        arming.left_armed = true;
    }

    Ok(())
}

fn clamp_norm(f: f64) -> u16 {
    let v = f.clamp(0.0, 1.0) * 10000.0;
    v.round() as u16
}
