//! hangar spike: the topic library (one writer, many readers, a ring of slots in a
//! shared-memory file) and the system interface the manager uses.
//!
//! `unsafe` lives in two places only: `sys` (the C library and raw system calls) and
//! the views of the shared mapping in `topic`.

pub mod sys;
pub mod topic;

pub use topic::{Info, Input, Publisher, ReadySet, Status, Subscriber, flag_writer_died, topic_dir, wait};
