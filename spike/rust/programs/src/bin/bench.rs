//! bench [--runs 5] [--count 5000000] [--seconds 10] [--messages 100000] [test...]
//!
//! Tests: publish publish16 latest torn lost_wake (default: all). Prints one JSON
//! object; each figure is the median of `--runs` runs (torn counts and timeouts are
//! summed over the runs, since they must be 0). Needs `$HANGAR_TOPIC_DIR`.
//!
//! The multi-process tests run this same binary as their writer and readers
//! (`bench --child <role> ...`).

use hangar::{Info, Publisher, Status, Subscriber, sys};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::process::{Child, ChildStdout, Command, ExitCode, Stdio};

const TESTS: [&str; 5] = ["publish", "publish16", "latest", "torn", "lost_wake"];

struct Opts {
    runs: usize,
    count: u64,
    seconds: f64,
    messages: u64,
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = if args.first().map(String::as_str) == Some("--child") {
        child(&args[1..])
    } else {
        parent(&args)
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("bench: {e}");
            ExitCode::FAILURE
        }
    }
}

fn bad(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, msg.into())
}

fn parent(args: &[String]) -> io::Result<()> {
    let mut o = Opts { runs: 5, count: 5_000_000, seconds: 10.0, messages: 100_000 };
    let mut tests: Vec<&str> = Vec::new();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().ok_or_else(|| bad(format!("{a} needs a value")));
        match a.as_str() {
            "--runs" => o.runs = val()?.parse().map_err(|_| bad("bad --runs"))?,
            "--count" => o.count = val()?.parse().map_err(|_| bad("bad --count"))?,
            "--seconds" => o.seconds = val()?.parse().map_err(|_| bad("bad --seconds"))?,
            "--messages" => o.messages = val()?.parse().map_err(|_| bad("bad --messages"))?,
            t if TESTS.contains(&t) => tests.push(TESTS[TESTS.iter().position(|x| *x == t).unwrap_or(0)]),
            other => return Err(bad(format!("unknown argument {other}; tests: {}", TESTS.join(" ")))),
        }
    }
    if o.runs == 0 || o.count == 0 {
        return Err(bad("--runs and --count must be positive"));
    }
    if tests.is_empty() {
        tests = TESTS.to_vec();
    }
    hangar::topic_dir()?;

    let mut out: Vec<String> = Vec::new();
    for t in tests {
        let field = match t {
            "publish" => format!("\"publish_ns\":{:.2}", median_f(o.runs, || bench_publish(256, o.count))?),
            "publish16" => format!("\"publish16_ns\":{:.2}", median_f(o.runs, || bench_publish(16, o.count))?),
            "latest" => format!("\"latest_ns\":{:.2}", median_f(o.runs, || bench_latest(o.count))?),
            "torn" => {
                let mut reads = Vec::new();
                let (mut torn, mut published) = (0, Vec::new());
                for _ in 0..o.runs {
                    let r = bench_torn(o.seconds)?;
                    reads.push(r.reads);
                    published.push(r.published);
                    torn += r.torn;
                }
                format!(
                    "\"torn\":{{\"published\":{},\"reads\":{},\"torn\":{torn}}}",
                    median_u(&mut published),
                    median_u(&mut reads)
                )
            }
            "lost_wake" => {
                let (mut received, mut gaps, mut timeouts) = (Vec::new(), Vec::new(), 0);
                for _ in 0..o.runs {
                    let r = bench_lost_wake(o.messages)?;
                    received.push(r.received);
                    gaps.push(r.gaps);
                    timeouts += r.timeouts;
                }
                format!(
                    "\"lost_wake\":{{\"sent\":{},\"received\":{},\"gaps\":{},\"timeouts\":{timeouts}}}",
                    o.messages,
                    median_u(&mut received),
                    median_u(&mut gaps)
                )
            }
            _ => unreachable!(),
        };
        out.push(field);
    }
    println!("{{{}}}", out.join(","));
    Ok(())
}

fn median_f(runs: usize, mut f: impl FnMut() -> io::Result<f64>) -> io::Result<f64> {
    let mut v = Vec::with_capacity(runs);
    for _ in 0..runs {
        v.push(f()?);
    }
    v.sort_by(f64::total_cmp);
    Ok(v[v.len() / 2])
}

fn median_u(v: &mut [u64]) -> u64 {
    v.sort_unstable();
    v[v.len() / 2]
}

fn pattern(buf: &mut [u8], seq: u64) {
    let s = seq as u8;
    for (i, b) in buf.iter_mut().enumerate() {
        *b = s ^ (i as u8);
    }
}

fn pattern_ok(buf: &[u8], seq: u64) -> bool {
    let s = seq as u8;
    buf.iter().enumerate().all(|(i, &b)| b == s ^ (i as u8))
}

/// ns per publish, nobody waiting.
fn bench_publish(payload: usize, count: u64) -> io::Result<f64> {
    let mut p = Publisher::open(if payload == 16 { "bench.publish16" } else { "bench.publish" }, payload, 16)?;
    let mut buf = vec![0u8; payload];
    pattern(&mut buf, 1);
    let t0 = sys::monotonic_ns();
    for _ in 0..count {
        p.publish(&buf, 0);
    }
    let t1 = sys::monotonic_ns();
    Ok((t1 - t0) as f64 / count as f64)
}

/// ns per latest(), as (publish + latest loop) minus (publish-only loop).
fn bench_latest(count: u64) -> io::Result<f64> {
    let mut p = Publisher::open("bench.latest", 256, 16)?;
    let mut s = Subscriber::open("bench.latest")?;
    let mut buf = vec![0u8; 256];
    let mut out = vec![0u8; 256];
    let mut info = Info::default();
    pattern(&mut buf, 1);
    let mut missed = 0u64;
    let t0 = sys::monotonic_ns();
    for _ in 0..count {
        p.publish(&buf, 0);
        if s.latest(&mut out, &mut info) != Status::Msg {
            missed += 1;
        }
    }
    let t1 = sys::monotonic_ns();
    for _ in 0..count {
        p.publish(&buf, 0);
    }
    let t2 = sys::monotonic_ns();
    if missed != 0 {
        return Err(io::Error::other(format!("latest missed {missed} messages")));
    }
    let both = (t1 - t0) as f64;
    let publish_only = (t2 - t1) as f64;
    Ok((both - publish_only) / count as f64)
}

/// A child process, killed and reaped when dropped, so that an early return leaves
/// nothing running.
struct Reaped(Child);

impl Drop for Reaped {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

struct Torn {
    published: u64,
    reads: u64,
    torn: u64,
}

fn spawn_child(args: &[&str], stdin: bool) -> io::Result<(Reaped, BufReader<ChildStdout>)> {
    let mut c = Command::new(std::env::current_exe()?)
        .arg("--child")
        .args(args)
        .stdin(if stdin { Stdio::piped() } else { Stdio::null() })
        .stdout(Stdio::piped())
        .spawn()?;
    let out = c.stdout.take().map(BufReader::new);
    let c = Reaped(c);
    Ok((c, out.ok_or_else(|| io::Error::other("no stdout"))?))
}

fn read_line(r: &mut BufReader<ChildStdout>) -> io::Result<String> {
    let mut s = String::new();
    r.read_line(&mut s)?;
    if s.is_empty() {
        return Err(io::Error::other("child ended early"));
    }
    Ok(s.trim().to_string())
}

fn numbers(line: &str) -> Vec<u64> {
    line.split_whitespace().filter_map(|w| w.parse().ok()).collect()
}

/// One writer process into a 4-slot ring, four readers checking every byte.
fn bench_torn(seconds: f64) -> io::Result<Torn> {
    let secs = seconds.to_string();
    let (mut w, mut wout) = spawn_child(&["torn-writer", "bench.torn", &secs], false)?;
    if read_line(&mut wout)? != "ready" {
        return Err(io::Error::other("torn writer did not start"));
    }
    let mut readers = Vec::new();
    for _ in 0..4 {
        readers.push(spawn_child(&["torn-reader", "bench.torn", &(seconds + 10.0).to_string()], false)?);
    }
    let published = numbers(&read_line(&mut wout)?).first().copied().unwrap_or(0);
    w.0.wait()?;
    let mut r = Torn { published, reads: 0, torn: 0 };
    for (mut c, mut out) in readers {
        let n = numbers(&read_line(&mut out)?);
        c.0.wait()?;
        r.reads += n.first().copied().unwrap_or(0);
        r.torn += n.get(1).copied().unwrap_or(0);
    }
    Ok(r)
}

struct LostWake {
    received: u64,
    gaps: u64,
    timeouts: u64,
}

/// A writer at random gaps of 0 to 200 µs; this process waits (10 s timeout) and
/// reads in order.
fn bench_lost_wake(messages: u64) -> io::Result<LostWake> {
    let (mut w, mut wout) = spawn_child(&["lw-writer", "bench.lost_wake", &messages.to_string()], true)?;
    if read_line(&mut wout)? != "ready" {
        return Err(io::Error::other("lost_wake writer did not start"));
    }
    let mut s = Subscriber::open("bench.lost_wake")?;
    if !s.is_attached() {
        return Err(io::Error::other("lost_wake topic missing"));
    }
    let mut stdin = w.0.stdin.take().ok_or_else(|| io::Error::other("no stdin"))?;
    stdin.write_all(b"g")?;
    drop(stdin);
    let mut r = LostWake { received: 0, gaps: 0, timeouts: 0 };
    let mut buf = vec![0u8; 256];
    let mut info = Info::default();
    'outer: loop {
        if hangar::wait(&mut [&mut s], 10_000)?.is_none() {
            r.timeouts += 1;
            if w.0.try_wait()?.is_some() {
                break;
            }
            continue;
        }
        loop {
            let st = s.next(&mut buf, &mut info);
            r.gaps += info.gap;
            match st {
                Status::Msg => r.received += 1,
                Status::Closed => break 'outer,
                _ => break,
            }
        }
    }
    let _ = read_line(&mut wout);
    w.0.wait()?;
    Ok(r)
}

// ---------------------------------------------------------------------------------
// Child processes

fn child(args: &[String]) -> io::Result<()> {
    let arg = |i: usize| args.get(i).map(String::as_str).ok_or_else(|| bad("child: missing argument"));
    let role = arg(0)?;
    let topic = arg(1)?;
    let num = arg(2)?;
    let mut stdout = io::stdout().lock();
    match role {
        "torn-writer" => {
            let secs: f64 = num.parse().map_err(|_| bad("seconds"))?;
            let mut p = Publisher::open(topic, 256, 4)?;
            writeln!(stdout, "ready")?;
            stdout.flush()?;
            let end = sys::monotonic_ns() + (secs * 1e9) as u64;
            let mut buf = [0u8; 256];
            let mut n = 0u64;
            loop {
                for _ in 0..1024 {
                    pattern(&mut buf, p.next_seq());
                    p.publish(&buf, 0);
                }
                n += 1024;
                if sys::monotonic_ns() >= end {
                    break;
                }
            }
            p.close();
            writeln!(stdout, "{n}")?;
        }
        "torn-reader" => {
            let secs: f64 = num.parse().map_err(|_| bad("seconds"))?;
            let end = sys::monotonic_ns() + (secs * 1e9) as u64;
            let mut s = Subscriber::open(topic)?;
            let mut buf = [0u8; 256];
            let mut info = Info::default();
            let (mut reads, mut torn) = (0u64, 0u64);
            let mut i = 0u64;
            loop {
                i += 1;
                let st = if i.is_multiple_of(2) { s.latest(&mut buf, &mut info) } else { s.next(&mut buf, &mut info) };
                match st {
                    Status::Msg => {
                        reads += 1;
                        if !pattern_ok(&buf, info.seq) {
                            torn += 1;
                        }
                    }
                    Status::Closed => break,
                    _ => {}
                }
                if i.is_multiple_of(4096) && sys::monotonic_ns() >= end {
                    break;
                }
            }
            writeln!(stdout, "{reads} {torn}")?;
        }
        "lw-writer" => {
            let messages: u64 = num.parse().map_err(|_| bad("messages"))?;
            let mut p = Publisher::open(topic, 256, 64)?;
            writeln!(stdout, "ready")?;
            stdout.flush()?;
            let mut go = [0u8; 1];
            io::stdin().read_exact(&mut go)?;
            let mut rng = sys::random_u64() | 1;
            let mut buf = [0u8; 256];
            for _ in 0..messages {
                pattern(&mut buf, p.next_seq());
                p.publish(&buf, 0);
                // xorshift64
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                sys::sleep_until_ns(sys::monotonic_ns() + rng % 200_001);
            }
            p.close();
            writeln!(stdout, "{messages}")?;
        }
        other => return Err(bad(format!("unknown child role {other}"))),
    }
    stdout.flush()
}
