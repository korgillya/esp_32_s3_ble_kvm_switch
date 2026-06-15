//! Windows cursor read/write via user32.

use anyhow::Result;
use windows::Win32::Foundation::POINT;
use windows::Win32::UI::WindowsAndMessaging::{
    GetCursorPos, GetSystemMetrics, SetCursorPos, SM_CXSCREEN, SM_CYSCREEN,
};

use crate::cursor::{Position, ScreenBounds};

pub fn current_cursor() -> Result<Position> {
    let mut p = POINT::default();
    unsafe { GetCursorPos(&mut p) }?;
    Ok(Position {
        x: p.x as f64,
        y: p.y as f64,
    })
}

pub fn primary_screen_bounds() -> Result<ScreenBounds> {
    // SM_CX/CY_SCREEN return the dimensions of the primary monitor in physical
    // pixels (or scaled, depending on the manifest). v1 ignores DPI scaling.
    let w = unsafe { GetSystemMetrics(SM_CXSCREEN) };
    let h = unsafe { GetSystemMetrics(SM_CYSCREEN) };
    Ok(ScreenBounds {
        width: w as f64,
        height: h as f64,
    })
}

pub fn warp_cursor(pos: Position) -> Result<()> {
    unsafe { SetCursorPos(pos.x as i32, pos.y as i32) }?;
    Ok(())
}
