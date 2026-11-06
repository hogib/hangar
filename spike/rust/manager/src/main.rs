//! spike-manager: starts the modules of a config file, restarts them when they exit,
//! and stops them on SIGTERM/SIGINT. One thread, one epoll.
//!
//! Config, one module per line: `<name> <topic-it-writes-or-'-'> <program> [args...]`.

use hangar::sys::{self, Epoll, EpollEvent, ExecImage, SignalFd, TimerFd};
use std::ffi::{OsStr, OsString, c_int};
use std::fmt::Write as _;
use std::fs;
use std::io;
use std::os::fd::{AsFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

const TOKEN_SIGNAL: u64 = 0;
const TOKEN_TIMER: u64 = 1;
const TOKEN_MODULE: u64 = 2;

const BACKOFF_FIRST_MS: u64 = 100;
const BACKOFF_MAX_MS: u64 = 2000;
const RESET_AFTER_NS: u64 = 5_000_000_000;
const STOP_TIMEOUT_NS: u64 = 500_000_000;

/// Logs a line to stderr. A failed write (the log reader went away: EPIPE, since Rust
/// ignores SIGPIPE) is dropped; `eprintln!` would panic and abort the supervisor.
macro_rules! log {
    ($($arg:tt)*) => {{
        use std::io::Write as _;
        let _ = writeln!(std::io::stderr(), "spike-manager: {}", format_args!($($arg)*));
    }};
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Running,
    Backoff,
    Stopping,
    Stopped,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            State::Running => "running",
            State::Backoff => "backoff",
            State::Stopping => "stopping",
            State::Stopped => "stopped",
        }
    }
}

struct Module {
    name: String,
    topic: Option<String>,
    program: String,
    image: ExecImage,
    state: State,
    pid: i32,
    starttime: u64,
    pidfd: Option<OwnedFd>,
    started_ns: u64,
    failures: u32,
    delay_ms: u64,
    restart_at: Option<u64>,
}

struct Manager {
    modules: Vec<Module>,
    epoll: Epoll,
    signals: SignalFd,
    timer: TimerFd,
    registry: PathBuf,
    /// Set once SIGTERM/SIGINT arrived.
    stopping: bool,
    /// While stopping, until it passes: the time to SIGKILL the modules still running.
    stop_deadline: Option<u64>,
}

fn main() -> ExitCode {
    let args: Vec<OsString> = std::env::args_os().collect();
    if args.len() != 2 {
        eprintln!("usage: spike-manager <config>");
        return ExitCode::from(2);
    }
    match run(Path::new(&args[1])) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            log!("{e}");
            ExitCode::FAILURE
        }
    }
}

fn run(config: &Path) -> io::Result<()> {
    let dir = hangar::topic_dir()?;
    fs::create_dir_all(&dir)?;
    let registry = dir.join("../registry");
    let text = fs::read_to_string(config)
        .map_err(|e| io::Error::new(e.kind(), format!("cannot read {}: {e}", config.display())))?;
    let modules = parse_config(&text)?;

    let signals = SignalFd::new(&[sys::SIGTERM, sys::SIGINT, sys::SIGCHLD])?;
    let timer = TimerFd::new()?;
    let epoll = Epoll::new()?;
    epoll.add(signals.fd(), TOKEN_SIGNAL)?;
    epoll.add(timer.fd(), TOKEN_TIMER)?;
    let mut m = Manager { modules, epoll, signals, timer, registry, stopping: false, stop_deadline: None };

    let result = m.supervise();
    if result.is_err() {
        m.kill_all();
    }
    let _ = fs::remove_file(&m.registry);
    if result.is_ok() {
        log!("all modules stopped");
    }
    result
}

fn parse_config(text: &str) -> io::Result<Vec<Module>> {
    let env: Vec<(OsString, OsString)> = std::env::vars_os().collect();
    let mut modules = Vec::new();
    for (lineno, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let words: Vec<&str> = line.split_whitespace().collect();
        if words.len() < 3 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("config line {}: want <name> <topic|-> <program> [args...]", lineno + 1),
            ));
        }
        let (name, topic, program) = (words[0], words[1], words[2]);
        if modules.iter().any(|m: &Module| m.name == name) {
            return Err(io::Error::new(io::ErrorKind::InvalidData, format!("module {name} twice")));
        }
        let path = resolve(program);
        let image = ExecImage::new(path.as_os_str(), &words[2..], env.iter().map(|(k, v)| (k, v)))?;
        modules.push(Module {
            name: name.to_string(),
            topic: (topic != "-").then(|| topic.to_string()),
            program: program.to_string(),
            image,
            state: State::Stopped,
            pid: 0,
            starttime: 0,
            pidfd: None,
            started_ns: 0,
            failures: 0,
            delay_ms: BACKOFF_FIRST_MS,
            restart_at: None,
        });
    }
    Ok(modules)
}

/// The program's path: as given if it has a slash, else the first executable match
/// in `$PATH` (or the bare name, so that exec reports "not found").
fn resolve(program: &str) -> PathBuf {
    if program.contains('/') {
        return PathBuf::from(program);
    }
    let path = std::env::var_os("PATH").unwrap_or_else(|| OsString::from("/usr/bin:/bin"));
    for dir in path.as_bytes().split(|&b| b == b':') {
        let dir = if dir.is_empty() { Path::new(".") } else { Path::new(OsStr::from_bytes(dir)) };
        let candidate = dir.join(program);
        if let Ok(meta) = fs::metadata(&candidate)
            && meta.is_file()
            && meta.permissions().mode() & 0o111 != 0
        {
            return candidate;
        }
    }
    PathBuf::from(program)
}

/// Field 22 of `/proc/<pid>/stat`: the start time in clock ticks.
fn starttime(pid: i32) -> u64 {
    let Ok(stat) = fs::read_to_string(format!("/proc/{pid}/stat")) else { return 0 };
    let Some(close) = stat.rfind(')') else { return 0 };
    // Field 3 is the first after the command name, so field 22 is the 20th.
    stat[close + 1..].split_whitespace().nth(19).and_then(|f| f.parse().ok()).unwrap_or(0)
}

impl Module {
    /// Sends `sig` to the module's process group and, through its pidfd, to the leader
    /// itself, which may have left the group.
    fn signal(&self, sig: c_int) {
        if let Some(pidfd) = &self.pidfd {
            let _ = sys::kill(-self.pid, sig);
            let _ = sys::pidfd_send_signal(pidfd.as_fd(), sig);
        }
    }

    /// Flags the topic the module writes as written by a dead writer.
    fn flag_topic(&self) {
        if let Some(topic) = &self.topic
            && let Err(e) = hangar::flag_writer_died(topic)
            && e.kind() != io::ErrorKind::NotFound
        {
            log!("{}: cannot flag topic {topic}: {e}", self.name);
        }
    }
}

impl Manager {
    /// Starts every module and runs the event loop until all are stopped. An error
    /// return leaves modules running; the caller kills them.
    fn supervise(&mut self) -> io::Result<()> {
        for i in 0..self.modules.len() {
            self.start(i)?;
        }
        self.write_registry();
        self.arm_timer();

        let mut events = [EpollEvent::ZERO; 16];
        loop {
            let n = self.epoll.wait(&mut events)?;
            for ev in &events[..n] {
                match ev.token() {
                    TOKEN_SIGNAL => self.on_signals()?,
                    TOKEN_TIMER => self.on_timer()?,
                    t => self.on_exit((t - TOKEN_MODULE) as usize),
                }
            }
            if self.stopping && self.modules.iter().all(|md| md.pidfd.is_none()) {
                return Ok(());
            }
            self.arm_timer();
        }
    }

    /// After a fatal error: SIGKILLs every module and flags the topics they write, so
    /// nothing is left running unsupervised. They are not reaped: once the manager
    /// exits, init reaps them.
    fn kill_all(&mut self) {
        for md in &mut self.modules {
            if md.pidfd.is_some() {
                md.signal(sys::SIGKILL);
                md.flag_topic();
                md.pidfd = None;
                md.state = State::Stopped;
            }
        }
        log!("every module killed");
    }

    /// Starts module `i`, or schedules a retry. An error means its exit could not be
    /// watched, which is fatal.
    fn start(&mut self, i: usize) -> io::Result<()> {
        let md = &mut self.modules[i];
        md.restart_at = None;
        match sys::spawn(&md.image) {
            Ok(child) => {
                md.pid = child.pid;
                md.starttime = starttime(child.pid);
                md.started_ns = sys::monotonic_ns();
                md.state = State::Running;
                let watched = self.epoll.add(child.pidfd.as_fd(), TOKEN_MODULE + i as u64);
                md.pidfd = Some(child.pidfd);
                watched.map_err(|e| io::Error::new(e.kind(), format!("{}: epoll: {e}", md.name)))?;
                match child.exec_error {
                    Some(e) => log!("{}: cannot run {}: {e}", md.name, md.program),
                    None => log!("{}: started, pid {}", md.name, md.pid),
                }
            }
            Err(e) => {
                log!("{}: cannot start: {e}", md.name);
                md.pid = 0;
                md.starttime = 0;
                md.started_ns = sys::monotonic_ns();
                schedule_restart(md);
            }
        }
        Ok(())
    }

    fn on_signals(&mut self) -> io::Result<()> {
        while let Some(sig) = self.signals.read()? {
            if (sig == sys::SIGTERM || sig == sys::SIGINT) && !self.stopping {
                log!("signal {sig}: stopping every module");
                self.stopping = true;
                self.stop_deadline = Some(sys::monotonic_ns() + STOP_TIMEOUT_NS);
                for md in &mut self.modules {
                    md.restart_at = None;
                    if md.pidfd.is_some() {
                        md.signal(sys::SIGTERM);
                        md.state = State::Stopping;
                    } else {
                        md.state = State::Stopped;
                    }
                }
                self.write_registry();
            }
            // SIGCHLD carries nothing the pidfds do not.
        }
        Ok(())
    }

    fn on_timer(&mut self) -> io::Result<()> {
        self.timer.drain();
        let now = sys::monotonic_ns();
        if self.stopping {
            if self.stop_deadline.is_some_and(|t| t <= now) {
                // Their exits arrive through the pidfds, as any other: nothing here
                // waits on a process that may never die (one in uninterruptible sleep).
                self.stop_deadline = None;
                for md in &self.modules {
                    md.signal(sys::SIGKILL);
                }
            }
            return Ok(());
        }
        let mut changed = false;
        for i in 0..self.modules.len() {
            let md = &self.modules[i];
            if md.state == State::Backoff && md.restart_at.is_some_and(|t| t <= now) {
                self.start(i)?;
                changed = true;
            }
        }
        if changed {
            self.write_registry();
        }
        Ok(())
    }

    /// A module's pidfd is readable: it has exited.
    fn on_exit(&mut self, i: usize) {
        let Some(md) = self.modules.get_mut(i) else { return };
        // Look without reaping, so the PID and process group id stay ours.
        let status = match md.pidfd.as_ref().map(|fd| sys::wait_pidfd(fd.as_fd(), false)) {
            None | Some(Ok(None)) => return,
            Some(Ok(Some(s))) => Ok(s),
            Some(Err(e)) => Err(e),
        };
        md.flag_topic();
        md.signal(sys::SIGKILL);
        let Some(pidfd) = md.pidfd.take() else { return };
        let status = match sys::wait_pidfd(pidfd.as_fd(), true) {
            Ok(Some(s)) => Ok(s),
            _ => status,
        };
        drop(pidfd);
        let status = match status {
            Ok(s) => s.to_string(),
            Err(e) => format!("ended (waitid: {e})"),
        };
        if self.stopping {
            md.state = State::Stopped;
            log!("{}: {status}", md.name);
        } else {
            schedule_restart(md);
            log!("{}: {status}; restart in {:.1} s", md.name, md.delay_ms as f64 / 1000.0);
        }
        self.write_registry();
    }

    /// Arms the timer for the earliest restart or the stop deadline; disarms it when
    /// there is neither (also once the deadline has passed), so an idle manager never
    /// wakes.
    fn arm_timer(&self) {
        let next = if self.stopping {
            self.stop_deadline
        } else {
            self.modules.iter().filter_map(|m| m.restart_at).min()
        };
        if let Err(e) = self.timer.set(next) {
            log!("timerfd: {e}");
        }
    }

    fn write_registry(&self) {
        let mut text = String::from("hangar-registry 1\n");
        for md in &self.modules {
            let _ = writeln!(
                text,
                "{} {} {} {} {} {}",
                md.name,
                md.pid,
                md.starttime,
                md.state.name(),
                md.failures,
                md.delay_ms as f64 / 1000.0
            );
        }
        let mut tmp = self.registry.clone().into_os_string();
        tmp.push(".tmp");
        let tmp = PathBuf::from(tmp);
        if let Err(e) = fs::write(&tmp, text).and_then(|()| fs::rename(&tmp, &self.registry)) {
            log!("cannot write {}: {e}", self.registry.display());
        }
    }
}

/// Counts a failure and sets the restart time: the first delay, doubled on each
/// further failure up to the maximum. A module that ran at least 5 s starts over.
fn schedule_restart(md: &mut Module) {
    let now = sys::monotonic_ns();
    if now.saturating_sub(md.started_ns) >= RESET_AFTER_NS {
        md.failures = 0;
        md.delay_ms = BACKOFF_FIRST_MS;
    } else if md.failures > 0 {
        md.delay_ms = (md.delay_ms * 2).min(BACKOFF_MAX_MS);
    }
    md.failures += 1;
    md.state = State::Backoff;
    md.restart_at = Some(now + md.delay_ms * 1_000_000);
}
