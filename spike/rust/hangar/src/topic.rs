//! Topics: a named stream of fixed-size messages, one writer, any number of readers,
//! a ring of slots in a shared-memory file (`docs/01-topics.md`, the subset in
//! `spike/SPEC.md`).
//!
//! Everything another process may write while a file is mapped is an atomic: the
//! header's `flags`, `futex_word`, `write_seq` and `last_publish_ns`, every slot
//! header field and every payload word. Payloads are copied as 64-bit relaxed atomic
//! loads and stores, so the seqlock is data-race free in the Rust memory model; the
//! before/after check on `seq` decides whether the copy is kept.
//!
//! Readers map the file `PROT_READ`, and on read-only memory the only atomic access
//! Rust allows is a `Relaxed` load of at most 8 bytes (`core::sync::atomic`, "Atomic
//! accesses to read-only memory"). A reader's ring is a `Ring<ReadOnly>`, which offers
//! only such loads, followed by fences where ordering is needed; the header itself is
//! handed out by a `Ring<ReadWrite>` alone.

use crate::sys::{self, Mapping, WaitList};
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions, Permissions};
use std::io;
use std::marker::PhantomData;
use std::mem::{offset_of, size_of};
use std::os::fd::AsFd;
use std::os::unix::fs::{FileExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering::{Acquire, Relaxed, Release, SeqCst};
use std::sync::atomic::{AtomicU32, AtomicU64, fence};

const MAGIC: [u8; 8] = *b"HGTOPIC1";
const VERSION: u32 = 1;
const HEADER_BYTES: usize = 4096;
const SLOT_HEADER_BYTES: usize = 128;
const WAKE_BYTES: usize = 64;
const MAX_NAME: usize = 47;
const MAX_SLOTS: u32 = 65536;

/// `flags` bit 0: the writer closed the topic.
pub const FLAG_CLOSED: u32 = 1;
/// `flags` bit 1: the writer died (set by the manager).
pub const FLAG_DIED: u32 = 2;
/// `flags` bit 2: a newer file replaced this one.
pub const FLAG_REPLACED: u32 = 4;
const FLAG_ENDED: u32 = FLAG_CLOSED | FLAG_DIED;

/// How often `wait` looks for a topic that does not exist yet, or for the successor of
/// an ended or replaced file that could not be opened. The spec has no directory word,
/// so these are the things that are polled.
const ABSENT_POLL_NS: u64 = 100_000_000;

/// The 4096-byte file header. Fields other than the atomics are written once, through
/// the file descriptor of a temporary file no other call knows, before the file is
/// mapped or renamed into place. Some fields are part of the format but unused by this
/// subset.
#[repr(C)]
#[allow(dead_code)]
struct Header {
    magic: [u8; 8],
    version: u32,
    header_bytes: u32,
    generation: u64,
    slot_count: u32,
    slot_header_bytes: u32,
    payload_bytes: u32,
    flags: AtomicU32,
    slot_stride: u64,
    slots_offset: u64,
    topic_id: u32,
    rate_mhz: u32,
    type_hash: u64,
    name: [u8; 48],
    writer: [u8; 32],
    writer_pid: i32,
    schema_bytes: u32,
    created_real_ns: u64,
    first_seq: u64,
    stale_after_ns: u64,
    _reserved0: u64,
    futex_word: AtomicU32,
    _reserved1: u32,
    write_seq: AtomicU64,
    last_publish_ns: AtomicU64,
    _rest: [u8; HEADER_BYTES - 216],
}

#[repr(C)]
struct InputRef {
    topic_id: AtomicU32,
    flags: AtomicU32,
    seq: AtomicU64,
}

/// The 128-byte slot header; the payload follows it.
#[repr(C)]
struct SlotHeader {
    seq: AtomicU64,
    stamp_ns: AtomicU64,
    publish_ns: AtomicU64,
    n_inputs: AtomicU32,
    flags: AtomicU32,
    _reserved: [AtomicU64; 4],
    inputs: [InputRef; 4],
}

const _: () = {
    assert!(size_of::<Header>() == HEADER_BYTES);
    assert!(offset_of!(Header, version) == 8);
    assert!(offset_of!(Header, generation) == 16);
    assert!(offset_of!(Header, slot_count) == 24);
    assert!(offset_of!(Header, payload_bytes) == 32);
    assert!(offset_of!(Header, flags) == 36);
    assert!(offset_of!(Header, slot_stride) == 40);
    assert!(offset_of!(Header, slots_offset) == 48);
    assert!(offset_of!(Header, topic_id) == 56);
    assert!(offset_of!(Header, name) == 72);
    assert!(offset_of!(Header, first_seq) == 168);
    assert!(offset_of!(Header, futex_word) == 192);
    assert!(offset_of!(Header, write_seq) == 200);
    assert!(offset_of!(Header, last_publish_ns) == 208);
    assert!(size_of::<SlotHeader>() == SLOT_HEADER_BYTES);
    assert!(offset_of!(SlotHeader, n_inputs) == 24);
    assert!(offset_of!(SlotHeader, inputs) == 64);
};

/// One provenance record of a message.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Input {
    pub topic_id: u32,
    pub flags: u32,
    pub seq: u64,
}

/// What a read returned besides the payload.
#[derive(Clone, Copy, Debug, Default)]
pub struct Info {
    pub seq: u64,
    pub stamp_ns: u64,
    pub publish_ns: u64,
    pub n_inputs: u32,
    pub flags: u32,
    pub inputs: [Input; 4],
    /// Messages this call found lost or skipped (set by every call, whatever it returns).
    pub gap: u64,
}

/// The result of a read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// A message was copied.
    Msg,
    /// Nothing new.
    None,
    /// The topic does not exist yet; a later call attaches when it appears.
    Absent,
    /// The writer closed the topic or died, and everything in it has been read. A new
    /// writer's file is picked up when it appears.
    Closed,
    /// The writer overtook the copy repeatedly.
    Busy,
    /// `buf` is shorter than `Subscriber::payload_bytes()`, which a replacing writer may
    /// have grown. Nothing was consumed: grow the buffer and call again.
    TooSmall,
    /// The topic exists but cannot be attached, or the file that replaced it cannot be
    /// opened; `Subscriber::last_error()` says why. Later calls, and `wait` every 100 ms,
    /// try again.
    Error,
}

/// FNV-1a, 32 bits.
pub fn topic_id(name: &str) -> u32 {
    name.bytes().fold(0x811c_9dc5u32, |h, b| (h ^ u32::from(b)).wrapping_mul(0x0100_0193))
}

/// Bytes from one slot to the next for a payload size.
pub fn slot_stride(payload_bytes: usize) -> usize {
    (SLOT_HEADER_BYTES + payload_bytes.next_multiple_of(8)).next_multiple_of(64)
}

fn invalid(what: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, what.to_string())
}

fn check_name(name: &str) -> io::Result<()> {
    let ok = !name.is_empty()
        && name.len() <= MAX_NAME
        && name.split('.').all(|seg| {
            !seg.is_empty() && seg.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
        });
    if ok {
        Ok(())
    } else {
        Err(io::Error::new(io::ErrorKind::InvalidInput, format!("bad topic name {name:?}")))
    }
}

/// `$HANGAR_TOPIC_DIR`, which is required.
pub fn topic_dir() -> io::Result<PathBuf> {
    match std::env::var_os("HANGAR_TOPIC_DIR") {
        Some(d) if !d.is_empty() => Ok(PathBuf::from(d)),
        _ => Err(io::Error::new(io::ErrorKind::NotFound, "HANGAR_TOPIC_DIR is not set")),
    }
}

fn hidden(dir: &Path, name: &str, suffix: &str) -> PathBuf {
    let mut s = OsString::from(".");
    s.push(name);
    s.push(suffix);
    dir.join(s)
}

// ---------------------------------------------------------------------------------
// Views of mapped memory: the only unsafe code in this module.

/// The header at the start of a mapping. On a read-only mapping the caller may read
/// its plain fields and make `Relaxed` loads of its atomics, nothing else (see
/// `Ring<ReadOnly>`).
fn header_of(map: &Mapping) -> &Header {
    assert!(map.len() >= HEADER_BYTES);
    // SAFETY: the mapping is page-aligned and at least HEADER_BYTES long (checked
    // above). `Header` is `repr(C)` of integers, byte arrays and atomics, so every bit
    // pattern is valid; its plain fields are never written while the file is mapped,
    // and the others are atomics, accessed only atomically (only by `Relaxed` loads
    // when the mapping is read-only). The reference borrows `map`, so it cannot outlive
    // the mapping.
    unsafe { &*map.as_ptr().cast::<Header>() }
}

/// The wake file's `wait_flag`, at offset 0.
fn wait_flag_of(map: &Mapping) -> &AtomicU32 {
    assert!(map.len() >= size_of::<AtomicU32>());
    // SAFETY: the mapping is page-aligned and holds at least 4 bytes; the word is only
    // ever accessed atomically, by every process; the reference borrows `map`.
    unsafe { &*map.as_ptr().cast::<AtomicU32>() }
}

/// A load-acquire that is allowed on read-only memory: a `Relaxed` load, then an
/// acquire fence.
fn load_acquire_u64(a: &AtomicU64) -> u64 {
    let v = a.load(Relaxed);
    fence(Acquire);
    v
}

/// As `load_acquire_u64`.
fn load_acquire_u32(a: &AtomicU32) -> u32 {
    let v = a.load(Relaxed);
    fence(Acquire);
    v
}

/// How a `Ring` maps its file.
trait Access {
    const WRITABLE: bool;
}

/// `PROT_READ`: the reader's mapping.
enum ReadOnly {}

/// `PROT_READ | PROT_WRITE`: the writer's mapping.
enum ReadWrite {}

impl Access for ReadOnly {
    const WRITABLE: bool = false;
}

impl Access for ReadWrite {
    const WRITABLE: bool = true;
}

/// A mapped, validated topic file.
struct Ring<A: Access> {
    map: Mapping,
    slot_mask: u64,
    payload_bytes: usize,
    payload_words: usize,
    stride: usize,
    /// (device, inode) of the file, to tell a replacement from the same file.
    id: (u64, u64),
    _access: PhantomData<A>,
}

impl<A: Access> Ring<A> {
    fn open(path: &Path) -> io::Result<Self> {
        let file = OpenOptions::new().read(true).write(A::WRITABLE).open(path)?;
        Ring::from_file(&file)
    }

    fn from_file(file: &File) -> io::Result<Self> {
        let meta = file.metadata()?;
        let len = usize::try_from(meta.len()).map_err(|_| invalid("topic file too large"))?;
        if len < HEADER_BYTES {
            return Err(invalid("topic file shorter than its header"));
        }
        let map = Mapping::new(file.as_fd(), len, A::WRITABLE)?;
        let h = header_of(&map);
        let k = h.slot_count;
        let payload_bytes = h.payload_bytes as usize;
        let stride = slot_stride(payload_bytes);
        if h.magic != MAGIC || h.version != VERSION {
            return Err(invalid("not a hangar topic file"));
        }
        if h.header_bytes as usize != HEADER_BYTES
            || h.slot_header_bytes as usize != SLOT_HEADER_BYTES
            || h.slots_offset != HEADER_BYTES as u64
            || !k.is_power_of_two()
            || !(2..=MAX_SLOTS).contains(&k)
            || payload_bytes == 0
            || h.slot_stride != stride as u64
        {
            return Err(invalid("topic header sizes are inconsistent"));
        }
        let need = HEADER_BYTES as u64 + u64::from(k) * stride as u64;
        if (len as u64) < need {
            return Err(invalid("topic file shorter than its slots"));
        }
        Ok(Ring {
            map,
            slot_mask: u64::from(k) - 1,
            payload_bytes,
            payload_words: payload_bytes.div_ceil(8),
            stride,
            id: (meta.dev(), meta.ino()),
            _access: PhantomData,
        })
    }

    fn slot_count(&self) -> u64 {
        self.slot_mask + 1
    }

    /// The slot that holds message `n` (n >= 1), and its payload words. On a
    /// `Ring<ReadOnly>` they are only read with `Relaxed` loads (`copy`).
    fn slot(&self, n: u64) -> (&SlotHeader, &[AtomicU64]) {
        let index = ((n.wrapping_sub(1)) & self.slot_mask) as usize;
        let offset = HEADER_BYTES + index * self.stride;
        debug_assert!(offset + self.stride <= self.map.len());
        // SAFETY: `from_file` checked that the file holds slot_count slots of `stride`
        // bytes after the header, so [offset, offset + stride) is inside the mapping;
        // `offset` is a multiple of 64 from a page-aligned base, so aligned for the
        // slot header and for u64 words; SLOT_HEADER_BYTES + 8 * payload_words <= stride
        // by the stride formula. Both types are made only of atomics, valid for any
        // bit pattern and accessed only atomically. The references borrow `self`.
        unsafe {
            let base = self.map.as_ptr().add(offset);
            let header = &*base.cast::<SlotHeader>();
            let words = std::slice::from_raw_parts(
                base.add(SLOT_HEADER_BYTES).cast::<AtomicU64>(),
                self.payload_words,
            );
            (header, words)
        }
    }
}

impl Ring<ReadWrite> {
    fn header(&self) -> &Header {
        header_of(&self.map)
    }
}

/// A reader's view: plain fields, and atomics read only with `Relaxed` loads.
impl Ring<ReadOnly> {
    fn first_seq(&self) -> u64 {
        header_of(&self.map).first_seq
    }

    fn write_seq(&self) -> u64 {
        load_acquire_u64(&header_of(&self.map).write_seq)
    }

    fn flags(&self) -> u32 {
        load_acquire_u32(&header_of(&self.map).flags)
    }

    /// The word to sleep on; `futex_waitv` may wait on read-only memory.
    fn futex_word(&self) -> &AtomicU32 {
        &header_of(&self.map).futex_word
    }

    /// Copies message `n` with the seqlock before/after check. `buf` holds at least
    /// `payload_bytes`.
    fn copy(&self, n: u64, buf: &mut [u8], info: &mut Info) -> Copy {
        let (s, words) = self.slot(n);
        let s1 = load_acquire_u64(&s.seq);
        if s1 != 2 * n {
            return Copy::Moved;
        }
        info.stamp_ns = s.stamp_ns.load(Relaxed);
        info.publish_ns = s.publish_ns.load(Relaxed);
        info.n_inputs = s.n_inputs.load(Relaxed);
        info.flags = s.flags.load(Relaxed);
        for (dst, src) in info.inputs.iter_mut().zip(&s.inputs) {
            *dst = Input {
                topic_id: src.topic_id.load(Relaxed),
                flags: src.flags.load(Relaxed),
                seq: src.seq.load(Relaxed),
            };
        }
        load_words(words, &mut buf[..self.payload_bytes]);
        fence(Acquire);
        if s.seq.load(Relaxed) != s1 {
            return Copy::Torn;
        }
        info.n_inputs = info.n_inputs.min(4);
        info.seq = n;
        Copy::Done
    }
}

#[derive(PartialEq, Eq)]
enum Copy {
    Done,
    /// The slot does not hold the message (yet or any more).
    Moved,
    /// Overwritten while it was copied.
    Torn,
}

fn load_words(words: &[AtomicU64], out: &mut [u8]) {
    let (chunks, tail) = out.as_chunks_mut::<8>();
    for (c, w) in chunks.iter_mut().zip(words) {
        *c = w.load(Relaxed).to_ne_bytes();
    }
    if !tail.is_empty() {
        let last = words[chunks.len()].load(Relaxed).to_ne_bytes();
        tail.copy_from_slice(&last[..tail.len()]);
    }
}

fn store_words(words: &[AtomicU64], data: &[u8]) {
    let (chunks, tail) = data.as_chunks::<8>();
    let mut rest = words.iter();
    for (c, w) in chunks.iter().zip(&mut rest) {
        w.store(u64::from_ne_bytes(*c), Relaxed);
    }
    if !tail.is_empty() {
        let mut last = [0u8; 8];
        last[..tail.len()].copy_from_slice(tail);
        if let Some(w) = rest.next() {
            w.store(u64::from_ne_bytes(last), Relaxed);
        }
    }
    for w in rest {
        w.store(0, Relaxed);
    }
}

fn open_wake(path: &Path, create: bool) -> io::Result<Mapping> {
    let file = OpenOptions::new().read(true).write(true).create(create).mode(0o660).open(path)?;
    if file.metadata()?.len() < WAKE_BYTES as u64 {
        if !create {
            return Err(invalid("wake file too short"));
        }
        file.set_len(WAKE_BYTES as u64)?;
    }
    if create {
        // The mode given to open is filtered by the umask; the spec wants 0660.
        let _ = file.set_permissions(Permissions::from_mode(0o660));
    }
    Mapping::new(file.as_fd(), WAKE_BYTES, true)
}

/// Ends a topic the way a close does: sets `bit` in `flags`, bumps `futex_word` and
/// wakes every reader.
fn announce(h: &Header, bit: u32, order: std::sync::atomic::Ordering) {
    h.flags.fetch_or(bit, order);
    h.futex_word.fetch_add(1, SeqCst);
    sys::futex_wake_all(&h.futex_word);
}

/// Marks the topic `name` as written by a writer that died (flags bit 1) and wakes its
/// readers. Used by the manager when the module writing it exits.
pub fn flag_writer_died(name: &str) -> io::Result<()> {
    check_name(name)?;
    let file = OpenOptions::new().read(true).write(true).open(topic_dir()?.join(name))?;
    if file.metadata()?.len() < HEADER_BYTES as u64 {
        return Err(invalid("topic file shorter than its header"));
    }
    let map = Mapping::new(file.as_fd(), HEADER_BYTES, true)?;
    let h = header_of(&map);
    if h.magic != MAGIC {
        return Err(invalid("not a hangar topic file"));
    }
    announce(h, FLAG_DIED, Release);
    Ok(())
}

// ---------------------------------------------------------------------------------
// Writer

/// The single writer of a topic.
pub struct Publisher {
    ring: Ring<ReadWrite>,
    wake: Mapping,
    write_seq: u64,
    closed: bool,
}

impl Publisher {
    /// Creates the topic file (replacing an existing one, whose sequence continues).
    pub fn open(name: &str, payload_bytes: usize, slot_count: u32) -> io::Result<Publisher> {
        check_name(name)?;
        if !slot_count.is_power_of_two() || !(2..=MAX_SLOTS).contains(&slot_count) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "slot count must be a power of two, 2 to 65536"));
        }
        if payload_bytes == 0 || payload_bytes > (1 << 30) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "bad payload size"));
        }
        let dir = topic_dir()?;
        let path = dir.join(name);
        // A missing or invalid file starts the sequence at 0. Any other failure (a topic
        // this process may not write, say) is returned: carrying on would restart the
        // sequence under its readers without telling them.
        let old = match Ring::<ReadWrite>::open(&path) {
            Ok(r) => Some(r),
            Err(e) if matches!(e.kind(), io::ErrorKind::NotFound | io::ErrorKind::InvalidData) => None,
            Err(e) => return Err(e),
        };
        let w = old.as_ref().map_or(0, |r| r.header().write_seq.load(Acquire));

        let stride = slot_stride(payload_bytes);
        let total = HEADER_BYTES + slot_count as usize * stride;
        let (tmp, file) = create_temp(&dir, name)?;
        let ring = (|| {
            sys::fallocate(file.as_fd(), total as u64)?;
            file.write_all_at(&header_bytes(name, payload_bytes, slot_count, w), 0)?;
            Ring::from_file(&file)
        })();
        let ring = match ring {
            Ok(r) => r,
            Err(e) => {
                let _ = fs::remove_file(&tmp);
                return Err(e);
            }
        };
        let wake = match open_wake(&hidden(&dir, name, ".wake"), true) {
            Ok(m) => m,
            Err(e) => {
                let _ = fs::remove_file(&tmp);
                return Err(e);
            }
        };
        if let Err(e) = fs::rename(&tmp, &path) {
            let _ = fs::remove_file(&tmp);
            return Err(e);
        }
        // Flagged once the new file is in place, so a reader that sees bit 2 always
        // finds its successor at the path.
        if let Some(old) = &old {
            announce(old.header(), FLAG_REPLACED, SeqCst);
        }
        Ok(Publisher { ring, wake, write_seq: w, closed: false })
    }

    /// The sequence number the next `publish` will give its message.
    pub fn next_seq(&self) -> u64 {
        self.write_seq + 1
    }

    pub fn payload_bytes(&self) -> usize {
        self.ring.payload_bytes
    }

    /// Publishes one message (`payload` at most `payload_bytes`, zero-padded) and
    /// returns its sequence number. Never blocks; makes a system call only if a reader
    /// is waiting.
    pub fn publish(&mut self, payload: &[u8], stamp_ns: u64) -> u64 {
        assert!(payload.len() <= self.ring.payload_bytes, "payload larger than the topic's");
        let n = self.write_seq + 1;
        let (s, words) = self.ring.slot(n);
        s.seq.store(2 * n - 1, Relaxed);
        fence(Release);
        let now = sys::monotonic_ns();
        s.stamp_ns.store(stamp_ns, Relaxed);
        s.publish_ns.store(now, Relaxed);
        s.n_inputs.store(0, Relaxed);
        s.flags.store(0, Relaxed);
        store_words(words, payload);
        s.seq.store(2 * n, Release);
        let h = self.ring.header();
        h.write_seq.store(n, Release);
        h.last_publish_ns.store(now, Relaxed);
        h.futex_word.fetch_add(1, SeqCst);
        if wait_flag_of(&self.wake).swap(0, SeqCst) != 0 {
            sys::futex_wake_all(&h.futex_word);
        }
        self.write_seq = n;
        n
    }

    /// Closes the topic: readers see `Closed` once they have read everything.
    pub fn close(mut self) {
        self.close_inner();
    }

    fn close_inner(&mut self) {
        if !self.closed {
            self.closed = true;
            announce(self.ring.header(), FLAG_CLOSED, SeqCst);
        }
    }
}

impl Drop for Publisher {
    fn drop(&mut self) {
        self.close_inner();
    }
}

/// Creates `<dir>/.<name>.tmp.<pid>.<random>`, a name no other call uses, so nothing
/// else opens or truncates the file while it is set up and mapped.
fn create_temp(dir: &Path, name: &str) -> io::Result<(PathBuf, File)> {
    let mut tries = 0;
    loop {
        let tmp = hidden(dir, name, &format!(".tmp.{}.{:016x}", std::process::id(), sys::random_u64()));
        match OpenOptions::new().read(true).write(true).create_new(true).mode(0o640).open(&tmp) {
            Ok(file) => return Ok((tmp, file)),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists && tries < 8 => tries += 1,
            Err(e) => return Err(e),
        }
    }
}

fn header_bytes(name: &str, payload_bytes: usize, slot_count: u32, write_seq: u64) -> Vec<u8> {
    let mut b = vec![0u8; HEADER_BYTES];
    let mut put = |off: usize, v: &[u8]| b[off..off + v.len()].copy_from_slice(v);
    put(offset_of!(Header, magic), &MAGIC);
    put(offset_of!(Header, version), &VERSION.to_ne_bytes());
    put(offset_of!(Header, header_bytes), &(HEADER_BYTES as u32).to_ne_bytes());
    put(offset_of!(Header, generation), &sys::random_u64().to_ne_bytes());
    put(offset_of!(Header, slot_count), &slot_count.to_ne_bytes());
    put(offset_of!(Header, slot_header_bytes), &(SLOT_HEADER_BYTES as u32).to_ne_bytes());
    put(offset_of!(Header, payload_bytes), &(payload_bytes as u32).to_ne_bytes());
    put(offset_of!(Header, slot_stride), &(slot_stride(payload_bytes) as u64).to_ne_bytes());
    put(offset_of!(Header, slots_offset), &(HEADER_BYTES as u64).to_ne_bytes());
    put(offset_of!(Header, topic_id), &topic_id(name).to_ne_bytes());
    put(offset_of!(Header, name), name.as_bytes());
    put(offset_of!(Header, first_seq), &write_seq.to_ne_bytes());
    put(offset_of!(Header, write_seq), &write_seq.to_ne_bytes());
    b
}

// ---------------------------------------------------------------------------------
// Reader

struct Attached {
    ring: Ring<ReadOnly>,
    wake: Mapping,
}

impl Attached {
    fn open(path: &Path, wake_path: &Path) -> io::Result<Attached> {
        let ring = Ring::open(path)?;
        let wake = open_wake(wake_path, false)?;
        Ok(Attached { ring, wake })
    }
}

/// A reader of one topic. Opening never blocks and never fails for a topic that does
/// not exist yet.
pub struct Subscriber {
    path: PathBuf,
    wake_path: PathBuf,
    att: Option<Attached>,
    /// The last sequence number returned (or skipped over).
    last: u64,
    /// The `flags` end bits (closed, died) already reported.
    closed_seen: u32,
    /// The current file's bit 2 was acted on, but no new file could be opened: `wait`
    /// looks at the path on the absent-topic poll instead of reporting the bit again.
    replaced_seen: bool,
    /// Why the topic could not be attached or reopened, until it can.
    error: Option<io::Error>,
    reopens: u64,
}

enum Drained {
    /// Messages arrived after all: read on.
    More,
    Reopened,
    Closed,
    /// The file that replaced this one cannot be opened.
    Failed,
    Nothing,
}

impl Subscriber {
    /// Subscribes to `name`. If the topic exists, reading starts with the next message
    /// published; if it appears later, with the first message of its file. A topic
    /// that exists but cannot be attached is an error.
    pub fn open(name: &str) -> io::Result<Subscriber> {
        check_name(name)?;
        let dir = topic_dir()?;
        let mut s = Subscriber {
            path: dir.join(name),
            wake_path: hidden(&dir, name, ".wake"),
            att: None,
            last: 0,
            closed_seen: 0,
            replaced_seen: false,
            error: None,
            reopens: 0,
        };
        match Attached::open(&s.path, &s.wake_path) {
            Ok(a) => {
                s.last = a.ring.write_seq();
                s.att = Some(a);
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e),
        }
        Ok(s)
    }

    /// Attaches to a topic that was absent; true if it now exists. Any failure other
    /// than its absence is kept in `error`.
    fn attach(&mut self) -> bool {
        match Attached::open(&self.path, &self.wake_path) {
            Ok(a) => {
                self.last = a.ring.first_seq();
                self.att = Some(a);
                self.error = None;
                true
            }
            Err(e) => {
                self.error = (e.kind() != io::ErrorKind::NotFound).then_some(e);
                false
            }
        }
    }

    /// What a read returns while the topic is not attached.
    fn unattached(&self) -> Status {
        if self.error.is_some() { Status::Error } else { Status::Absent }
    }

    /// Whether the topic exists (has been attached).
    pub fn is_attached(&self) -> bool {
        self.att.is_some()
    }

    /// The topic's payload size, once attached: the buffer size reads need.
    pub fn payload_bytes(&self) -> Option<usize> {
        self.att.as_ref().map(|a| a.ring.payload_bytes)
    }

    /// Why the last read returned `Status::Error`, while the problem lasts.
    pub fn last_error(&self) -> Option<&io::Error> {
        self.error.as_ref()
    }

    /// How many times a replaced file was reopened.
    pub fn reopens(&self) -> u64 {
        self.reopens
    }

    /// Moves to the file now at the path, if it is a different one. A failure to open
    /// it is kept in `error`.
    fn try_reopen(&mut self) -> bool {
        let Some(att) = &self.att else { return false };
        let current = att.ring.id;
        let opened = match fs::metadata(&self.path) {
            Ok(m) if (m.dev(), m.ino()) == current => Ok(None),
            Ok(_) => Attached::open(&self.path, &self.wake_path).map(Some),
            Err(e) => Err(e),
        };
        match opened {
            Ok(Some(a)) if a.ring.id != current => {
                // A writer that did not find the old file starts its sequence over:
                // carry on from its start rather than wait for it to catch up.
                self.last = self.last.min(a.ring.first_seq());
                self.att = Some(a);
                self.closed_seen = 0;
                self.replaced_seen = false;
                self.error = None;
                self.reopens += 1;
                true
            }
            Ok(_) => {
                self.error = None;
                false
            }
            Err(e) => {
                self.error = (e.kind() != io::ErrorKind::NotFound).then_some(e);
                false
            }
        }
    }

    /// Whether the current file holds a message newer than the last one returned.
    fn unread(&self) -> bool {
        self.att.as_ref().is_some_and(|a| a.ring.write_seq() > self.last.max(a.ring.first_seq()))
    }

    /// Called when the current file looked drained.
    fn after_drain(&mut self) -> Drained {
        let Some(att) = &self.att else { return Drained::Nothing };
        let flags = att.ring.flags();
        if flags & (FLAG_REPLACED | FLAG_ENDED) == 0 {
            return Drained::Nothing;
        }
        // Every flag is set after the writer's last `write_seq` store, so this second
        // look sees every message: drain them before leaving the file.
        if self.unread() {
            return Drained::More;
        }
        if self.try_reopen() {
            return Drained::Reopened;
        }
        self.closed_seen = flags & FLAG_ENDED;
        self.replaced_seen = flags & FLAG_REPLACED != 0;
        if self.error.is_some() {
            Drained::Failed
        } else if self.closed_seen != 0 {
            Drained::Closed
        } else {
            Drained::Nothing
        }
    }

    /// The newest message, if it is newer than the last one returned.
    pub fn latest(&mut self, buf: &mut [u8], info: &mut Info) -> Status {
        info.gap = 0;
        loop {
            if self.att.is_none() && !self.attach() {
                return self.unattached();
            }
            let Some(att) = &self.att else { return Status::Absent };
            let ring = &att.ring;
            if buf.len() < ring.payload_bytes {
                return Status::TooSmall;
            }
            let mut tries = 0;
            let found = loop {
                let n = ring.write_seq();
                if n <= self.last || n <= ring.first_seq() {
                    break None;
                }
                if ring.copy(n, buf, info) == Copy::Done {
                    break Some(n);
                }
                tries += 1;
                if tries == 8 {
                    return Status::Busy;
                }
            };
            if let Some(n) = found {
                self.last = n;
                return Status::Msg;
            }
            match self.after_drain() {
                Drained::More | Drained::Reopened => continue,
                Drained::Closed => return Status::Closed,
                Drained::Failed => return Status::Error,
                Drained::Nothing => return Status::None,
            }
        }
    }

    /// The next message in order; `info.gap` counts the messages lost before it.
    pub fn next(&mut self, buf: &mut [u8], info: &mut Info) -> Status {
        info.gap = 0;
        loop {
            if self.att.is_none() && !self.attach() {
                return self.unattached();
            }
            let Some(att) = &self.att else { return Status::Absent };
            let ring = &att.ring;
            if buf.len() < ring.payload_bytes {
                return Status::TooSmall;
            }
            let k = ring.slot_count();
            let first = ring.first_seq();
            if self.last < first {
                // Messages of an older file that were never read.
                info.gap += first - self.last;
                self.last = first;
            }
            let mut w = self.last + 1;
            loop {
                let top = ring.write_seq();
                if w > top {
                    break;
                }
                if top >= k && w <= top - k + 1 {
                    // Gone, or the slot the writer fills next.
                    info.gap += top - k + 2 - w;
                    w = top - k + 2;
                }
                if ring.copy(w, buf, info) == Copy::Done {
                    self.last = w;
                    return Status::Msg;
                }
                // Odd, newer, or overwritten mid-copy: lost. Never retry the same one.
                info.gap += 1;
                self.last = w;
                w += 1;
            }
            match self.after_drain() {
                Drained::More | Drained::Reopened => continue,
                Drained::Closed => return Status::Closed,
                Drained::Failed => return Status::Error,
                Drained::Nothing => return Status::None,
            }
        }
    }

    /// Whether a read would return something other than `None` right now. The caller
    /// orders this after its `wait_flag` stores with a `SeqCst` fence.
    fn pending(&self) -> bool {
        let Some(att) = &self.att else { return false };
        if self.unread() {
            return true;
        }
        let flags = att.ring.flags();
        (flags & FLAG_REPLACED != 0 && !self.replaced_seen) || flags & FLAG_ENDED & !self.closed_seen != 0
    }

    /// Whether `wait` must look at the path itself: the topic is absent, or its file
    /// ended or was replaced and the file at the path could not be opened.
    fn polls(&self) -> bool {
        self.att.is_none() || self.replaced_seen || self.error.is_some()
    }
}

/// The subscribers `wait` found with news: bit i for `subs[i]`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReadySet(pub u128);

impl ReadySet {
    pub fn contains(&self, i: usize) -> bool {
        i < 128 && self.0 & (1 << i) != 0
    }
}

/// Sleeps until one of `subs` may have news (a message, a close, a replacement, or
/// an absent topic appearing), or `timeout_ms` passes (`None`). At most 128. A topic
/// that is absent, or whose replacing file cannot be opened, is looked for every
/// 100 ms.
pub fn wait(subs: &mut [&mut Subscriber], timeout_ms: u64) -> io::Result<Option<ReadySet>> {
    assert!(subs.len() <= WaitList::MAX, "wait takes at most 128 subscribers");
    let deadline = sys::monotonic_ns().saturating_add(timeout_ms.saturating_mul(1_000_000));
    loop {
        let mut ready = 0u128;
        let mut poll = false;
        for (i, s) in subs.iter_mut().enumerate() {
            if !s.polls() {
                continue;
            }
            let found = if s.att.is_none() { s.attach() } else { s.try_reopen() };
            if found {
                ready |= 1 << i;
            } else {
                poll = true;
            }
        }
        if ready != 0 {
            return Ok(Some(ReadySet(ready)));
        }
        let mut list = WaitList::new();
        for att in subs.iter().filter_map(|s| s.att.as_ref()) {
            let word = att.ring.futex_word();
            list.push(word, load_acquire_u32(word));
        }
        for att in subs.iter().filter_map(|s| s.att.as_ref()) {
            wait_flag_of(&att.wake).store(1, SeqCst);
        }
        // Store-load barrier: a publish either finds a flag set or is seen below.
        fence(SeqCst);
        for (i, s) in subs.iter().enumerate() {
            if s.pending() {
                ready |= 1 << i;
            }
        }
        if ready != 0 {
            return Ok(Some(ReadySet(ready)));
        }
        let now = sys::monotonic_ns();
        if now >= deadline {
            return Ok(None);
        }
        let until = if poll { deadline.min(now + ABSENT_POLL_NS) } else { deadline };
        if list.is_empty() {
            sys::sleep_until_ns(until);
        } else {
            list.wait(until)?;
        }
    }
}
