//! talker <topic> <rate_hz> [--payload 256] [--crash-after N]
//!
//! Publishes at a fixed rate, each payload filled with the byte `seq & 0xff`; with
//! `--crash-after N`, aborts after N messages.

use hangar::Publisher;
use hangar::sys;
use std::process::ExitCode;

const SLOTS: u32 = 16;

fn usage() -> ExitCode {
    eprintln!("usage: talker <topic> <rate_hz> [--payload 256] [--crash-after N]");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 {
        return usage();
    }
    let topic = &args[0];
    let Ok(rate) = args[1].parse::<f64>() else { return usage() };
    if rate.is_nan() || rate <= 0.0 {
        return usage();
    }
    let mut payload = 256usize;
    let mut crash_after: Option<u64> = None;
    let mut rest = args[2..].iter();
    while let Some(a) = rest.next() {
        let v = rest.next().and_then(|v| v.parse::<u64>().ok());
        match (a.as_str(), v) {
            ("--payload", Some(v)) if v > 0 => payload = v as usize,
            ("--crash-after", Some(v)) => crash_after = Some(v),
            _ => return usage(),
        }
    }

    let mut p = match Publisher::open(topic, payload, SLOTS) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("talker: cannot open {topic}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let period_ns = (1e9 / rate) as u64;
    let mut buf = vec![0u8; payload];
    let mut t = sys::monotonic_ns();
    let mut sent = 0u64;
    loop {
        buf.fill(p.next_seq() as u8);
        p.publish(&buf, sys::monotonic_ns());
        sent += 1;
        if crash_after == Some(sent) {
            std::process::abort();
        }
        t += period_ns;
        sys::sleep_until_ns(t);
    }
}
