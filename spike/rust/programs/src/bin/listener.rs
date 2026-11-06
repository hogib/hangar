//! listener <topic> <seconds>
//!
//! Waits with `wait()`, reads in order with `next()`, records each message's latency
//! (wake-up time minus `publish_ns`), checks every payload byte equals `seq & 0xff`,
//! and prints one line of JSON at the end.

use hangar::{Info, Status, Subscriber, sys};
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (Some(topic), Some(Ok(secs))) = (args.first(), args.get(1).map(|s| s.parse::<f64>())) else {
        eprintln!("usage: listener <topic> <seconds>");
        return ExitCode::from(2);
    };
    if args.len() != 2 || secs.is_nan() || secs < 0.0 {
        eprintln!("usage: listener <topic> <seconds>");
        return ExitCode::from(2);
    }
    let mut sub = match Subscriber::open(topic) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("listener: cannot open {topic}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let end = sys::monotonic_ns() + (secs * 1e9) as u64;
    // Sized by the topic: `next` says `TooSmall` when a replacing writer grows it.
    let mut buf: Vec<u8> = Vec::new();
    let mut info = Info::default();
    let mut lat_ns: Vec<u64> = Vec::with_capacity(1 << 16);
    let (mut gaps, mut torn) = (0u64, 0u64);
    loop {
        let now = sys::monotonic_ns();
        if now >= end {
            break;
        }
        match hangar::wait(&mut [&mut sub], (end - now).div_ceil(1_000_000)) {
            Ok(Some(_)) => {}
            Ok(None) => continue,
            Err(e) => {
                eprintln!("listener: wait: {e}");
                return ExitCode::FAILURE;
            }
        }
        let woke = sys::monotonic_ns();
        loop {
            let st = sub.next(&mut buf, &mut info);
            gaps += info.gap;
            match st {
                Status::Msg => {}
                Status::TooSmall => {
                    buf.resize(sub.payload_bytes().unwrap_or(0), 0);
                    continue;
                }
                _ => break,
            }
            lat_ns.push(woke.saturating_sub(info.publish_ns));
            let want = info.seq as u8;
            if buf[..sub.payload_bytes().unwrap_or(0)].iter().any(|&b| b != want) {
                torn += 1;
            }
        }
    }
    lat_ns.sort_unstable();
    let pct = |p: f64| -> f64 {
        if lat_ns.is_empty() {
            return 0.0;
        }
        let i = ((lat_ns.len() - 1) as f64 * p).round() as usize;
        lat_ns[i] as f64 / 1000.0
    };
    println!(
        "{{\"msgs\":{},\"gaps\":{},\"torn\":{},\"p50_us\":{:.1},\"p99_us\":{:.1},\"max_us\":{:.1},\"reopens\":{}}}",
        lat_ns.len(),
        gaps,
        torn,
        pct(0.50),
        pct(0.99),
        pct(1.0),
        sub.reopens()
    );
    ExitCode::SUCCESS
}
