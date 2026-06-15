//! Platform-abstracted cursor read/write + screen bounds.

use anyhow::Result;

use crate::ble::EntrySide;
use crate::platform;

#[derive(Clone, Copy, Debug)]
pub struct Position {
    pub x: f64,
    pub y: f64,
}

#[derive(Clone, Copy, Debug)]
pub struct ScreenBounds {
    pub width: f64,
    pub height: f64,
}

pub fn current_position() -> Result<Position> {
    platform::current_cursor()
}

pub fn primary_screen() -> Result<ScreenBounds> {
    platform::primary_screen_bounds()
}

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
