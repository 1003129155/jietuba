//! C ABI over the repository's Rust libraries. Each module mirrors the interface the
//! archived Python build had for that crate.
//!
//! Conventions: strings are NUL-terminated UTF-8; output text goes into caller
//! buffers, truncated on a character boundary; handles are freed by their `_free`;
//! a panic returns the export's failure value (see `panic`).

mod clipboard;
mod gif;
mod hdr;
mod longstitch;
mod panic;
mod smart_erase;
mod text;
mod video;
mod ppocr;
