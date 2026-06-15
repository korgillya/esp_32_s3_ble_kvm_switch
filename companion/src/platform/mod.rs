#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "macos")]
pub use macos::{current_cursor, primary_screen_bounds, warp_cursor};

#[cfg(target_os = "windows")]
mod windows;
#[cfg(target_os = "windows")]
pub use windows::{current_cursor, primary_screen_bounds, warp_cursor};

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
mod fallback {
    use anyhow::Result;

    use crate::cursor::{Position, ScreenBounds};

    pub fn current_cursor() -> Result<Position> {
        anyhow::bail!("cursor read unsupported on this platform yet");
    }

    pub fn primary_screen_bounds() -> Result<ScreenBounds> {
        anyhow::bail!("screen bounds unsupported on this platform yet");
    }

    pub fn warp_cursor(_pos: Position) -> Result<()> {
        anyhow::bail!("cursor warp unsupported on this platform yet");
    }
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
pub use fallback::{current_cursor, primary_screen_bounds, warp_cursor};
