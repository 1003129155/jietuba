use std::ffi::{c_char, CStr};

/// Borrowed UTF-8 from C; None for null or invalid UTF-8.
pub unsafe fn str_arg<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    CStr::from_ptr(ptr).to_str().ok()
}

/// A "+"-joined list such as "ctrl+shift"; empty for none.
pub unsafe fn plus_list(ptr: *const c_char) -> Option<Vec<String>> {
    Some(
        str_arg(ptr)?
            .split('+')
            .map(str::trim)
            .filter(|part| !part.is_empty())
            .map(str::to_owned)
            .collect(),
    )
}

/// An array of `count` C strings.
pub unsafe fn str_array(ptr: *const *const c_char, count: usize) -> Option<Vec<String>> {
    if count == 0 {
        return Some(Vec::new());
    }
    if ptr.is_null() {
        return None;
    }
    std::slice::from_raw_parts(ptr, count)
        .iter()
        .map(|item| str_arg(*item).map(str::to_owned))
        .collect()
}

/// Copies text into a C buffer of `len` bytes, NUL-terminated and cut on a char boundary.
pub unsafe fn write_text(text: &str, buffer: *mut c_char, len: usize) {
    if buffer.is_null() || len == 0 {
        return;
    }
    let mut end = text.len().min(len - 1);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    std::ptr::copy_nonoverlapping(text.as_ptr(), buffer.cast::<u8>(), end);
    *buffer.add(end) = 0;
}
