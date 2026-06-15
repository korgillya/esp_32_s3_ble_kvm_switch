//! macOS cursor read/write via Quartz (CoreGraphics).

use anyhow::Result;
use core_graphics::display::{CGDisplay, CGPoint};
use core_graphics::event::CGEvent;
use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};

use crate::cursor::{Position, ScreenBounds};

pub fn current_cursor() -> Result<Position> {
    let src = CGEventSource::new(CGEventSourceStateID::CombinedSessionState)
        .map_err(|_| anyhow::anyhow!("CGEventSource::new failed"))?;
    let event = CGEvent::new(src)
        .map_err(|_| anyhow::anyhow!("CGEvent::new failed"))?;
    let p = event.location();
    Ok(Position { x: p.x, y: p.y })
}

pub fn primary_screen_bounds() -> Result<ScreenBounds> {
    let main = CGDisplay::main();
    Ok(ScreenBounds {
        width: main.pixels_wide() as f64,
        height: main.pixels_high() as f64,
    })
}

pub fn warp_cursor(pos: Position) -> Result<()> {
    let p = CGPoint::new(pos.x, pos.y);
    // CGWarpMouseCursorPosition moves the cursor without generating events.
    CGDisplay::warp_mouse_cursor_position(p)
        .map_err(|rc| anyhow::anyhow!("CGWarpMouseCursorPosition rc={}", rc))?;
    // Release mouse capture after warp so subsequent motion is fluid.
    CGDisplay::associate_mouse_and_mouse_cursor_position(true)
        .map_err(|rc| anyhow::anyhow!("CGAssociateMouseAndMouseCursorPosition rc={}", rc))?;
    Ok(())
}
