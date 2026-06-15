//! Platform-abstracted cursor read/write + screen bounds.
//!
//! Wraps `core-graphics` (macOS) and `windows-rs` (Windows) so the rest of
//! the daemon can stay platform-agnostic. Coordinates are in *logical*
//! display pixels of the primary screen — DPI scaling and multi-monitor
//! handling are TODO for v2.

use anyhow::Result;

use crate::ble::EntrySide;
use crate::platform;

/// Logical screen-space coordinates.
#[derive(Clone, Copy, Debug)]
pub struct Position {
    pub x: f64,
    pub y: f64,
}

/// Dimensions of the primary screen (in the same coordinate space as
/// [`Position`]).
#[derive(Clone, Copy, Debug)]
pub struct ScreenBounds {
    pub width: f64,
    pub height: f64,
}

/// Read the current global cursor position from the OS.
pub fn current_position() -> Result<Position> {
    platform::current_cursor()
}

/// Width / height of the primary display.
pub fn primary_screen() -> Result<ScreenBounds> {
    platform::primary_screen_bounds()
}

/// Move the cursor to the entry point on this host's screen specified by a
/// `warp_cmd` notification from the firmware.
///
/// `pos_norm` is the fixed-point fraction (0..10000) of the orthogonal axis
/// — see `docs/gatt-contract.md`.
pub fn warp(entry: EntrySide, pos_norm: u16) -> Result<()> {
    let bounds = primary_screen()?;
    let frac = (pos_norm as f64) / 10000.0;
    let (x, y) = match entry {
        EntrySide::FromLeft => (0.0, frac * bounds.height),
        EntrySide::FromRight => (bounds.width - 1.0, frac * bounds.height),
        EntrySide::FromTop => (frac * bounds.width, 0.0),
        EntrySide::FromBottom => (frac * bounds.width, bounds.height - 1.0),
    };
    platform::warp_cursor(Position { x, y })
}
