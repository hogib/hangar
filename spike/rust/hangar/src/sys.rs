//! The system interface: the C library functions and raw system calls hangar needs,
//! declared by hand (no `libc` crate), each behind a safe wrapper.
//!
//! Everything here is Linux on x86_64 or aarch64 with musl. The two architectures
//! share every constant and structure used below except two system call numbers and
//! the packing of `struct epoll_event`.

use std::ffi::{CString, OsStr, c_char, c_int, c_long, c_uint, c_void};
use std::fs::File;
use std::io::{self, Read};
use std::marker::PhantomData;
use std::mem::size_of;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::ptr::{self, NonNull};
use std::sync::atomic::AtomicU32;

mod ffi {
    use super::{EpollEvent, Itimerspec, SigSet, Timespec};
    use std::ffi::{c_char, c_int, c_long, c_uint, c_void};

    unsafe extern "C" {
        pub fn syscall(number: c_long, ...) -> c_long;
        pub fn mmap(addr: *mut c_void, len: usize, prot: c_int, flags: c_int, fd: c_int, off: i64)
        -> *mut c_void;
        pub fn munmap(addr: *mut c_void, len: usize) -> c_int;
        pub safe fn posix_fallocate(fd: c_int, offset: i64, len: i64) -> c_int;
        pub fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
        pub fn clock_nanosleep(clock: c_int, flags: c_int, req: *const Timespec, rem: *mut Timespec)
        -> c_int;
        pub fn getrandom(buf: *mut c_void, len: usize, flags: c_uint) -> isize;
        pub safe fn kill(pid: c_int, sig: c_int) -> c_int;
        pub fn sigemptyset(set: *mut SigSet) -> c_int;
        pub fn sigaddset(set: *mut SigSet, sig: c_int) -> c_int;
        pub fn sigprocmask(how: c_int, set: *const SigSet, old: *mut SigSet) -> c_int;
        pub fn signalfd(fd: c_int, mask: *const SigSet, flags: c_int) -> c_int;
        pub safe fn timerfd_create(clock: c_int, flags: c_int) -> c_int;
        pub fn timerfd_settime(fd: c_int, flags: c_int, new: *const Itimerspec, old: *mut Itimerspec)
        -> c_int;
        pub safe fn epoll_create1(flags: c_int) -> c_int;
        pub fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, event: *mut EpollEvent) -> c_int;
        pub fn epoll_wait(epfd: c_int, events: *mut EpollEvent, max: c_int, timeout: c_int) -> c_int;
        pub safe fn setpgid(pid: c_int, pgid: c_int) -> c_int;
        pub fn execve(path: *const c_char, argv: *const *const c_char, envp: *const *const c_char)
        -> c_int;
        pub fn write(fd: c_int, buf: *const c_void, len: usize) -> isize;
        pub safe fn _exit(code: c_int) -> !;
    }
}

/// System call numbers. The newer calls (424 and up) share one numbering on every
/// architecture; the older ones do not.
mod nr {
    use std::ffi::c_long;
    #[cfg(target_arch = "x86_64")]
    pub const RT_SIGACTION: c_long = 13;
    #[cfg(target_arch = "x86_64")]
    pub const RT_SIGPROCMASK: c_long = 14;
    #[cfg(target_arch = "x86_64")]
    pub const FUTEX: c_long = 202;
    #[cfg(target_arch = "x86_64")]
    pub const WAITID: c_long = 247;
    #[cfg(target_arch = "aarch64")]
    pub const RT_SIGACTION: c_long = 134;
    #[cfg(target_arch = "aarch64")]
    pub const RT_SIGPROCMASK: c_long = 135;
    #[cfg(target_arch = "aarch64")]
    pub const FUTEX: c_long = 98;
    #[cfg(target_arch = "aarch64")]
    pub const WAITID: c_long = 95;
    pub const PIDFD_SEND_SIGNAL: c_long = 424;
    pub const CLONE3: c_long = 435;
    pub const CLOSE_RANGE: c_long = 436;
    pub const FUTEX_WAITV: c_long = 449;
}

pub const SIGINT: c_int = 2;
pub const SIGKILL: c_int = 9;
pub const SIGTERM: c_int = 15;
pub const SIGCHLD: c_int = 17;

const EINTR: c_int = 4;
const EAGAIN: c_int = 11;
const ESRCH: c_int = 3;
const ETIMEDOUT: c_int = 110;
const O_CLOEXEC: c_int = 0o2000000;
const O_NONBLOCK: c_int = 0o4000;
const CLOCK_MONOTONIC: c_int = 1;
const TIMER_ABSTIME: c_int = 1;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 1;
const FUTEX_WAKE: c_long = 1;
const FUTEX2_SIZE_U32: u32 = 2;
const SIG_BLOCK: c_int = 0;
const SIG_SETMASK: c_int = 2;
const SIG_DFL: usize = 0;
/// One past the highest signal number.
const NSIG: c_int = 65;
const EPOLL_CTL_ADD: c_int = 1;
const EPOLLIN: u32 = 1;
const CLONE_PIDFD: u64 = 0x1000;
const P_PIDFD: c_long = 3;
const WNOHANG: c_long = 1;
const WEXITED: c_long = 4;
const WNOWAIT: c_long = 0x0100_0000;
const CLD_EXITED: i32 = 1;
const CLD_KILLED: i32 = 2;
const CLD_DUMPED: i32 = 3;

#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

impl Timespec {
    fn from_ns(ns: u64) -> Self {
        Timespec { tv_sec: (ns / 1_000_000_000) as i64, tv_nsec: (ns % 1_000_000_000) as i64 }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Itimerspec {
    interval: Timespec,
    value: Timespec,
}

/// musl's `sigset_t` (128 bytes).
#[repr(C)]
struct SigSet([u64; 16]);

/// `siginfo_t` as `waitid` fills it for a child (128 bytes).
#[repr(C)]
#[derive(Default)]
struct SigInfo {
    signo: i32,
    errno: i32,
    code: i32,
    _pad: i32,
    pid: i32,
    uid: u32,
    status: i32,
    _rest: [u32; 25],
}

/// `struct epoll_event`, which the x86_64 kernel ABI packs.
#[cfg_attr(target_arch = "x86_64", repr(C, packed))]
#[cfg_attr(not(target_arch = "x86_64"), repr(C))]
#[derive(Clone, Copy)]
pub struct EpollEvent {
    events: u32,
    data: u64,
}

impl EpollEvent {
    pub const ZERO: EpollEvent = EpollEvent { events: 0, data: 0 };
    pub fn token(&self) -> u64 {
        self.data
    }
}

#[repr(C)]
#[derive(Default)]
struct CloneArgs {
    flags: u64,
    pidfd: u64,
    child_tid: u64,
    parent_tid: u64,
    exit_signal: u64,
    stack: u64,
    stack_size: u64,
    tls: u64,
    set_tid: u64,
    set_tid_size: u64,
    cgroup: u64,
}

/// The kernel's `struct sigaction`, which has `sa_restorer` on x86_64 and aarch64 alike,
/// and a 64-bit mask.
#[repr(C)]
struct KernelSigaction {
    handler: usize,
    flags: u64,
    restorer: usize,
    mask: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct FutexWaitv {
    val: u64,
    uaddr: u64,
    flags: u32,
    reserved: u32,
}

const _: () = {
    assert!(size_of::<SigSet>() == 128);
    assert!(size_of::<SigInfo>() == 128);
    assert!(size_of::<CloneArgs>() == 88);
    assert!(size_of::<FutexWaitv>() == 24);
    assert!(size_of::<KernelSigaction>() == 32);
    #[cfg(target_arch = "x86_64")]
    assert!(size_of::<EpollEvent>() == 12);
    #[cfg(not(target_arch = "x86_64"))]
    assert!(size_of::<EpollEvent>() == 16);
};

fn cvt(r: c_int) -> io::Result<c_int> {
    if r < 0 { Err(io::Error::last_os_error()) } else { Ok(r) }
}

fn cvt_long(r: c_long) -> io::Result<c_long> {
    if r < 0 { Err(io::Error::last_os_error()) } else { Ok(r) }
}

/// Takes ownership of a descriptor a C call just returned.
fn owned(fd: c_int) -> OwnedFd {
    // SAFETY: called only with a fresh, valid descriptor that nothing else owns.
    unsafe { OwnedFd::from_raw_fd(fd) }
}

// ---------------------------------------------------------------------------------
// Time and randomness

/// `CLOCK_MONOTONIC` in nanoseconds.
pub fn monotonic_ns() -> u64 {
    let mut ts = Timespec::default();
    // SAFETY: `ts` is a valid, writable timespec.
    unsafe { ffi::clock_gettime(CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

/// Sleeps until `CLOCK_MONOTONIC` reaches `t_ns`.
pub fn sleep_until_ns(t_ns: u64) {
    let ts = Timespec::from_ns(t_ns);
    // SAFETY: `ts` is a valid timespec; no remainder is wanted with TIMER_ABSTIME.
    while unsafe { ffi::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, ptr::null_mut()) }
        == EINTR
    {}
}

/// 64 random bits from the kernel.
pub fn random_u64() -> u64 {
    let mut v = [0u8; 8];
    // SAFETY: `v` is 8 writable bytes.
    let n = unsafe { ffi::getrandom(v.as_mut_ptr().cast(), v.len(), 0) };
    if n == 8 {
        u64::from_ne_bytes(v)
    } else {
        monotonic_ns() ^ (u64::from(std::process::id()) << 40)
    }
}

// ---------------------------------------------------------------------------------
// Files and shared memory

/// Reserves `len` bytes of a file (`posix_fallocate`).
pub fn fallocate(fd: BorrowedFd<'_>, len: u64) -> io::Result<()> {
    match ffi::posix_fallocate(fd.as_raw_fd(), 0, len as i64) {
        0 => Ok(()),
        e => Err(io::Error::from_raw_os_error(e)),
    }
}

/// A `MAP_SHARED` mapping of a whole file, unmapped on drop.
pub struct Mapping {
    ptr: NonNull<u8>,
    len: usize,
}

// SAFETY: a mapping is plain shared memory, valid from any thread; what is stored in it
// is accessed through atomics (or never changes), which the views over it enforce.
unsafe impl Send for Mapping {}
// SAFETY: as above.
unsafe impl Sync for Mapping {}

impl Mapping {
    pub fn new(fd: BorrowedFd<'_>, len: usize, writable: bool) -> io::Result<Mapping> {
        if len == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "empty mapping"));
        }
        let prot = if writable { PROT_READ | PROT_WRITE } else { PROT_READ };
        // SAFETY: a new mapping at an address the kernel chooses aliases no Rust object.
        let p = unsafe { ffi::mmap(ptr::null_mut(), len, prot, MAP_SHARED, fd.as_raw_fd(), 0) };
        if p as isize == -1 {
            return Err(io::Error::last_os_error());
        }
        let ptr = NonNull::new(p.cast()).ok_or_else(io::Error::last_os_error)?;
        Ok(Mapping { ptr, len })
    }

    pub fn len(&self) -> usize {
        self.len
    }

    /// Always false: an empty mapping cannot be made.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn as_ptr(&self) -> *mut u8 {
        self.ptr.as_ptr()
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        // SAFETY: exactly the region mmap returned; every view into it borrows `self`,
        // so none outlives this call.
        unsafe { ffi::munmap(self.ptr.as_ptr().cast(), self.len) };
    }
}

// ---------------------------------------------------------------------------------
// Futexes (always shared: the words live in files mapped by several processes)

/// `FUTEX_WAKE` every waiter on `word`.
pub fn futex_wake_all(word: &AtomicU32) {
    // SAFETY: FUTEX_WAKE uses the address only as a key; `word` is a valid u32.
    unsafe {
        ffi::syscall(nr::FUTEX, word.as_ptr() as c_long, FUTEX_WAKE, i32::MAX as c_long, 0 as c_long,
            0 as c_long, 0 as c_long)
    };
}

/// How a `futex_waitv` call ended.
pub enum Waitv {
    /// The futex at this index of the list was woken.
    Woken(usize),
    /// A word no longer held its expected value, or a signal interrupted the wait.
    Changed,
    TimedOut,
}

/// Up to 128 `(word, expected value)` pairs for one `futex_waitv` call.
pub struct WaitList<'a> {
    entries: [FutexWaitv; 128],
    len: usize,
    _words: PhantomData<&'a AtomicU32>,
}

impl<'a> WaitList<'a> {
    pub const MAX: usize = 128;

    pub fn new() -> Self {
        let empty = FutexWaitv { val: 0, uaddr: 0, flags: 0, reserved: 0 };
        WaitList { entries: [empty; 128], len: 0, _words: PhantomData }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn push(&mut self, word: &'a AtomicU32, expected: u32) {
        assert!(self.len < Self::MAX, "futex_waitv takes at most 128 futexes");
        self.entries[self.len] = FutexWaitv {
            val: u64::from(expected),
            uaddr: word.as_ptr() as u64,
            flags: FUTEX2_SIZE_U32,
            reserved: 0,
        };
        self.len += 1;
    }

    /// Sleeps until one of the words is woken, or `CLOCK_MONOTONIC` reaches
    /// `deadline_ns`.
    pub fn wait(&self, deadline_ns: u64) -> io::Result<Waitv> {
        let ts = Timespec::from_ns(deadline_ns);
        // SAFETY: the first `len` entries point at u32 words borrowed for 'a, which
        // outlives this call; `ts` is a valid timespec.
        let r = unsafe {
            ffi::syscall(nr::FUTEX_WAITV, self.entries.as_ptr() as c_long, self.len as c_long,
                0 as c_long, &ts as *const Timespec as c_long, CLOCK_MONOTONIC as c_long, 0 as c_long)
        };
        if r >= 0 {
            return Ok(Waitv::Woken(r as usize));
        }
        let e = io::Error::last_os_error();
        match e.raw_os_error() {
            Some(EAGAIN) | Some(EINTR) => Ok(Waitv::Changed),
            Some(ETIMEDOUT) => Ok(Waitv::TimedOut),
            _ => Err(e),
        }
    }
}

impl Default for WaitList<'_> {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------------
// Signals, timers, epoll

/// Sends `sig` to a process (`pid` > 0) or a process group (`pid` < 0). A target
/// that no longer exists is not an error.
pub fn kill(pid: i32, sig: c_int) -> io::Result<()> {
    if ffi::kill(pid, sig) == 0 {
        return Ok(());
    }
    let e = io::Error::last_os_error();
    if e.raw_os_error() == Some(ESRCH) { Ok(()) } else { Err(e) }
}

/// Sends `sig` to the process a pidfd refers to, wherever its process group is. A
/// process that has already exited is not an error.
pub fn pidfd_send_signal(pidfd: BorrowedFd<'_>, sig: c_int) -> io::Result<()> {
    // SAFETY: plain integer arguments; no siginfo is passed.
    let r = unsafe {
        ffi::syscall(nr::PIDFD_SEND_SIGNAL, pidfd.as_raw_fd() as c_long, sig as c_long, 0 as c_long,
            0 as c_long, 0 as c_long, 0 as c_long)
    };
    match cvt_long(r) {
        Ok(_) => Ok(()),
        Err(e) if e.raw_os_error() == Some(ESRCH) => Ok(()),
        Err(e) => Err(e),
    }
}

/// Like `kill`, but reports a target that does not exist as `false`.
pub fn kill_exists(pid: i32, sig: c_int) -> bool {
    ffi::kill(pid, sig) == 0
}

/// A `signalfd` for some signals, which are blocked in this thread.
pub struct SignalFd(File);

impl SignalFd {
    pub fn new(signals: &[c_int]) -> io::Result<SignalFd> {
        let mut set = SigSet([0; 16]);
        // SAFETY: `set` is a valid sigset_t for all three calls; the old mask is not wanted.
        let fd = unsafe {
            ffi::sigemptyset(&mut set);
            for &s in signals {
                ffi::sigaddset(&mut set, s);
            }
            cvt(ffi::sigprocmask(SIG_BLOCK, &set, ptr::null_mut()))?;
            cvt(ffi::signalfd(-1, &set, O_CLOEXEC | O_NONBLOCK))?
        };
        Ok(SignalFd(File::from(owned(fd))))
    }

    /// The next pending signal, if any.
    pub fn read(&self) -> io::Result<Option<c_int>> {
        let mut info = [0u8; 128];
        match (&self.0).read(&mut info) {
            Ok(128) => Ok(Some(i32::from_ne_bytes([info[0], info[1], info[2], info[3]]))),
            Ok(_) => Ok(None),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(None),
            Err(e) => Err(e),
        }
    }

    pub fn fd(&self) -> BorrowedFd<'_> {
        std::os::fd::AsFd::as_fd(&self.0)
    }
}

/// A `CLOCK_MONOTONIC` timerfd armed with absolute one-shot deadlines.
pub struct TimerFd(File);

impl TimerFd {
    pub fn new() -> io::Result<TimerFd> {
        let fd = cvt(ffi::timerfd_create(CLOCK_MONOTONIC, O_CLOEXEC | O_NONBLOCK))?;
        Ok(TimerFd(File::from(owned(fd))))
    }

    /// Arms the timer for `CLOCK_MONOTONIC` time `at_ns`, or disarms it.
    pub fn set(&self, at_ns: Option<u64>) -> io::Result<()> {
        let spec = Itimerspec {
            interval: Timespec::default(),
            value: at_ns.map(|t| Timespec::from_ns(t.max(1))).unwrap_or_default(),
        };
        // SAFETY: `spec` is a valid itimerspec; the old value is not wanted.
        cvt(unsafe { ffi::timerfd_settime(self.0.as_raw_fd(), TIMER_ABSTIME, &spec, ptr::null_mut()) })?;
        Ok(())
    }

    /// Consumes an expiry, if there is one.
    pub fn drain(&self) {
        let mut buf = [0u8; 8];
        let _ = (&self.0).read(&mut buf);
    }

    pub fn fd(&self) -> BorrowedFd<'_> {
        std::os::fd::AsFd::as_fd(&self.0)
    }
}

pub struct Epoll(OwnedFd);

impl Epoll {
    pub fn new() -> io::Result<Epoll> {
        Ok(Epoll(owned(cvt(ffi::epoll_create1(O_CLOEXEC))?)))
    }

    /// Watches `fd` for readability; events for it carry `token`.
    pub fn add(&self, fd: BorrowedFd<'_>, token: u64) -> io::Result<()> {
        let mut ev = EpollEvent { events: EPOLLIN, data: token };
        // SAFETY: `ev` is a valid epoll_event; both descriptors are open.
        cvt(unsafe { ffi::epoll_ctl(self.0.as_raw_fd(), EPOLL_CTL_ADD, fd.as_raw_fd(), &mut ev) })?;
        Ok(())
    }

    /// Waits without a timeout; returns how many entries of `events` were filled.
    pub fn wait(&self, events: &mut [EpollEvent]) -> io::Result<usize> {
        let max = events.len().min(i32::MAX as usize) as c_int;
        // SAFETY: the kernel writes at most `max` entries into `events`.
        let r = unsafe { ffi::epoll_wait(self.0.as_raw_fd(), events.as_mut_ptr(), max, -1) };
        match cvt(r) {
            Ok(n) => Ok(n as usize),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => Ok(0),
            Err(e) => Err(e),
        }
    }
}

// ---------------------------------------------------------------------------------
// Processes

/// How a child ended.
#[derive(Clone, Copy, Debug)]
pub enum ExitStatus {
    Exited(i32),
    Signaled { signal: i32, core_dumped: bool },
    Unknown,
}

impl std::fmt::Display for ExitStatus {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match *self {
            ExitStatus::Exited(c) => write!(f, "exited with status {c}"),
            ExitStatus::Signaled { signal, core_dumped: false } => write!(f, "killed by signal {signal}"),
            ExitStatus::Signaled { signal, core_dumped: true } => {
                write!(f, "killed by signal {signal} (core dumped)")
            }
            ExitStatus::Unknown => write!(f, "ended (status unknown)"),
        }
    }
}

/// `waitid(P_PIDFD, pidfd, WEXITED | WNOHANG)`, with `WNOWAIT` unless `reap`. Never
/// blocks: `None` if the process has not exited.
pub fn wait_pidfd(pidfd: BorrowedFd<'_>, reap: bool) -> io::Result<Option<ExitStatus>> {
    let mut info = SigInfo::default();
    let options = if reap { WEXITED | WNOHANG } else { WEXITED | WNOHANG | WNOWAIT };
    loop {
        // SAFETY: `info` is a writable 128-byte siginfo_t; no rusage is asked for.
        let r = unsafe {
            ffi::syscall(nr::WAITID, P_PIDFD, pidfd.as_raw_fd() as c_long,
                &mut info as *mut SigInfo as c_long, options, 0 as c_long, 0 as c_long)
        };
        match cvt_long(r) {
            Ok(_) => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    if info.pid == 0 {
        return Ok(None);
    }
    Ok(Some(match info.code {
        CLD_EXITED => ExitStatus::Exited(info.status),
        CLD_KILLED => ExitStatus::Signaled { signal: info.status, core_dumped: false },
        CLD_DUMPED => ExitStatus::Signaled { signal: info.status, core_dumped: true },
        _ => ExitStatus::Unknown,
    }))
}

/// A program, its arguments and its environment, converted for `execve` ahead of
/// time: nothing is allocated between `clone3` and `exec`.
pub struct ExecImage {
    path: CString,
    _strings: Vec<CString>,
    argv: Vec<*const c_char>,
    envp: Vec<*const c_char>,
}

impl ExecImage {
    pub fn new<A, K, V>(path: &OsStr, args: &[A], env: impl IntoIterator<Item = (K, V)>) -> io::Result<Self>
    where
        A: AsRef<OsStr>,
        K: AsRef<OsStr>,
        V: AsRef<OsStr>,
    {
        let cstr = |b: &[u8]| {
            CString::new(b).map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "NUL in argument"))
        };
        let path = cstr(path.as_bytes())?;
        let mut strings = Vec::new();
        for a in args {
            strings.push(cstr(a.as_ref().as_bytes())?);
        }
        let n_args = strings.len();
        for (k, v) in env {
            let mut kv = k.as_ref().as_bytes().to_vec();
            kv.push(b'=');
            kv.extend_from_slice(v.as_ref().as_bytes());
            strings.push(cstr(&kv)?);
        }
        // The CStrings' heap buffers do not move when `strings` is moved.
        let mut argv: Vec<*const c_char> = strings[..n_args].iter().map(|s| s.as_ptr()).collect();
        argv.push(ptr::null());
        let mut envp: Vec<*const c_char> = strings[n_args..].iter().map(|s| s.as_ptr()).collect();
        envp.push(ptr::null());
        Ok(ExecImage { path, _strings: strings, argv, envp })
    }
}

/// A child started by `spawn`.
pub struct Child {
    pub pid: i32,
    pub pidfd: OwnedFd,
    /// Set when the child could not exec; it has then exited with status 127.
    pub exec_error: Option<io::Error>,
}

/// Starts `image` in a new process with `clone3(CLONE_PIDFD)`. The child leads its
/// own process group, has default signal dispositions, an empty signal mask and only
/// descriptors 0 to 2. An exec failure comes back over a close-on-exec pipe.
///
/// Callable from a multi-threaded process: between `clone3` and `exec` the child makes
/// only system calls, so no lock another thread held at the time can stop it.
pub fn spawn(image: &ExecImage) -> io::Result<Child> {
    let (mut err_rd, err_wr) = io::pipe()?;
    let mut pidfd: c_int = -1;
    let args = CloneArgs {
        flags: CLONE_PIDFD,
        pidfd: &mut pidfd as *mut c_int as u64,
        exit_signal: SIGCHLD as u64,
        ..CloneArgs::default()
    };
    // SAFETY: clone3 without CLONE_VM gives the child its own copy of the address
    // space, as fork does, with only this thread in it. The child runs `exec_child`
    // alone, which is async-signal-safe, so whatever other threads were doing (holding
    // the allocator's or musl's locks) does not matter. `args` and `pidfd` are valid.
    let pid = unsafe {
        ffi::syscall(nr::CLONE3, &args as *const CloneArgs as c_long, size_of::<CloneArgs>() as c_long,
            0 as c_long, 0 as c_long, 0 as c_long, 0 as c_long)
    };
    if pid == 0 {
        // SAFETY: this is the new child, between clone3 and exec.
        unsafe { exec_child(image, err_wr.as_raw_fd()) }
    }
    cvt_long(pid)?;
    // The child does this too; whichever runs first wins, so the group exists before
    // the parent could signal it. EACCES once the child has exec'd is harmless.
    ffi::setpgid(pid as c_int, pid as c_int);
    drop(err_wr);
    let pidfd = owned(pidfd);
    let mut errno = [0u8; 4];
    let mut got = 0;
    while got < 4 {
        match err_rd.read(&mut errno[got..]) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => break,
        }
    }
    let exec_error = (got == 4).then(|| io::Error::from_raw_os_error(i32::from_ne_bytes(errno)));
    Ok(Child { pid: pid as i32, pidfd, exec_error })
}

/// The child's side of `spawn`: async-signal-safe, with no allocation and no lock. The
/// C library is used only for thin system call wrappers (`setpgid`, `execve`, `write`,
/// `_exit`); signal dispositions and the mask are set with raw system calls, since
/// musl's `sigaction` takes a lock for `SIGABRT`.
///
/// # Safety
/// Must be called only in a child just created by `clone3` without `CLONE_VM`.
unsafe fn exec_child(image: &ExecImage, err_fd: c_int) -> ! {
    ffi::setpgid(0, 0);
    let default = KernelSigaction { handler: SIG_DFL, flags: 0, restorer: 0, mask: 0 };
    let empty: u64 = 0;
    // SAFETY: `default` and `empty` are valid kernel sigaction and sigset values of the
    // sizes passed (SIGKILL and SIGSTOP fail harmlessly); the ranges closed hold no
    // descriptor this child still uses except `err_fd`, which is kept (it is
    // close-on-exec); execve gets NUL-terminated arrays of C strings that `image` keeps
    // alive; `write` reads 4 bytes of a local.
    unsafe {
        for sig in 1..NSIG {
            ffi::syscall(nr::RT_SIGACTION, sig as c_long, &default as *const KernelSigaction as c_long,
                0 as c_long, size_of::<u64>() as c_long, 0 as c_long, 0 as c_long);
        }
        ffi::syscall(nr::RT_SIGPROCMASK, SIG_SETMASK as c_long, &empty as *const u64 as c_long,
            0 as c_long, size_of::<u64>() as c_long, 0 as c_long, 0 as c_long);
        if err_fd > 3 {
            ffi::syscall(nr::CLOSE_RANGE, 3 as c_long, (err_fd - 1) as c_long, 0 as c_long,
                0 as c_long, 0 as c_long, 0 as c_long);
        }
        ffi::syscall(nr::CLOSE_RANGE, (err_fd + 1).max(3) as c_long, c_uint::MAX as c_long,
            0 as c_long, 0 as c_long, 0 as c_long, 0 as c_long);
        ffi::execve(image.path.as_ptr(), image.argv.as_ptr(), image.envp.as_ptr());
        let errno = io::Error::last_os_error().raw_os_error().unwrap_or(0).to_ne_bytes();
        ffi::write(err_fd, errno.as_ptr() as *const c_void, errno.len());
    }
    ffi::_exit(127)
}
