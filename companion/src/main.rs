use std::time::Duration;

use anyhow::{bail, Result};
use futures_util::StreamExt;
use tokio::time::{self, MissedTickBehavior};
use tracing::{error, info, warn};
use tracing_subscriber::EnvFilter;

mod ble;
mod cursor;
mod edge;
mod platform;

#[derive(Debug)]
struct Args {
    device_name: String,
    poll_hz: u32,
}

impl Args {
    fn parse() -> Result<Self> {
        let mut device_name = String::from("ESP32 KVM Mouse");
        let mut poll_hz: u32 = 60;
        let mut iter = std::env::args().skip(1);
        while let Some(arg) = iter.next() {
            match arg.as_str() {
                "--device-name" => {
                    device_name = iter
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--device-name needs a value"))?;
                }
                "--poll-hz" => {
                    let v = iter
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--poll-hz needs a value"))?;
                    poll_hz = v.parse().map_err(|e| anyhow::anyhow!("--poll-hz: {}", e))?;
                }
                "-h" | "--help" => {
                    print_usage();
                    std::process::exit(0);
                }
                other => bail!("unknown arg: {}", other),
            }
        }
        Ok(Self {
            device_name,
            poll_hz,
        })
    }
}

fn print_usage() {
    println!(
        "esp32-kvm-companion\n\
         \n\
         OPTIONS:\n  \
            --device-name <name>   BLE name to scan for (default: \"ESP32 KVM Mouse\")\n  \
            --poll-hz <n>          Cursor polling rate in Hz (default: 60)\n  \
            -h, --help             Show this message"
    );
}

/* bluest's CoreBluetooth backend dispatches operations onto Apple's BT
 * dispatch queue and tracks state per-thread; running our futures on a
 * multi-threaded tokio runtime causes `write_without_response` to fail with
 * "device isn't connected" when the await resumes on a different worker.
 * Using a single-threaded runtime keeps everything on one thread. */
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .init();

    let args = Args::parse()?;
    info!(device_name = %args.device_name, "starting esp32-kvm-companion");

    if let Err(e) = run(args).await {
        error!(error = ?e, "fatal");
        std::process::exit(1);
    }
    Ok(())
}

async fn run(args: Args) -> Result<()> {
    let (session, mut warp_stream, adapter) =
        ble::connect(&args.device_name).await?;
    info!(slot = ?session.slot, "connected, slot resolved");

    let bounds = cursor::primary_screen()?;
    info!(width = bounds.width, height = bounds.height, "primary screen bounds");

    let interval = Duration::from_millis(
        (1000u64 / u64::from(args.poll_hz.max(1))).max(1),
    );
    let mut tick = time::interval(interval);
    tick.set_missed_tick_behavior(MissedTickBehavior::Skip);
    let mut arming = edge::EdgeArming::ready();

    let mut keepalive = time::interval(Duration::from_secs(2));
    keepalive.set_missed_tick_behavior(MissedTickBehavior::Skip);
    // burn the first immediate tick
    keepalive.tick().await;

    loop {
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {
                info!("ctrl-c, shutting down");
                break;
            }
            notif = warp_stream.next() => {
                match notif {
                    Some(Ok(value)) => ble::handle_warp_value(&value),
                    Some(Err(e)) => warn!(error = ?e, "warp_cmd notify error"),
                    None => {
                        warn!("warp stream ended");
                        break;
                    }
                }
            }
            _ = keepalive.tick() => {
                ble::ensure_connected(&adapter, &session).await;
            }
            _ = tick.tick() => {
                if let Err(e) = edge::poll_once(&session, bounds, &mut arming).await {
                    warn!(error = ?e, "edge poll error");
                }
            }
        }
    }
    Ok(())
}
