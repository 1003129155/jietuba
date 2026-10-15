//! C ABI over the reference Rust libraries. Each module mirrors one crate's Python
//! binding (`python.rs`) so the C++ side calls the same code the Python build does.
//!
//! Conventions: strings are NUL-terminated UTF-8; output text goes into caller
//! buffers, truncated on a character boundary; handles are freed by their `_free`;
//! a panic returns the export's failure value (see `panic`).

mod clipboard;
mod gif;
mod hdr;
mod input_hub;
mod longstitch;
mod panic;
mod text;
mod video;
mod ppocr;
