//! pyclipboard for C. Mirrors the reference Python binding's ClipboardManager and the
//! module functions the application uses (set_clipboard_text, MAX_STITCHED_PIXELS).
//! Items and groups cross as JSON with the binding's attribute names; failures return
//! false with the binding's exception message in the error buffer.

use std::ffi::{c_char, c_void, CString};
use std::sync::Arc;

use pyclipboard::manager::{self, ClipboardManager, MultiPasteOptions};
use pyclipboard::multi_paste;
use pyclipboard::types::{PyClipboardItem, PyGroup};

use crate::panic::{guard, guard_error};
use crate::text::{str_arg, write_text};

pub struct JbClipboard {
    inner: ClipboardManager,
}

/// Receives each recorded item as JSON on the watcher thread; the text is valid only during the call.
pub type JbClipboardItemCallback = Option<unsafe extern "C" fn(item_json: *const c_char, user: *mut c_void)>;

struct CallbackTarget {
    callback: unsafe extern "C" fn(*const c_char, *mut c_void),
    user: *mut c_void,
}

// SAFETY: the caller keeps `user` valid and callable from the watcher thread until
// jb_clipboard_stop_monitor returns.
unsafe impl Send for CallbackTarget {}
unsafe impl Sync for CallbackTarget {}

impl CallbackTarget {
    fn notify(&self, item: &PyClipboardItem) {
        if let Ok(json) = CString::new(item_json(item).replace('\0', "")) {
            // SAFETY: see the Send impl above.
            unsafe { (self.callback)(json.as_ptr(), self.user) };
        }
    }
}

fn owned_text(text: &str) -> *mut c_char {
    CString::new(text.replace('\0', "")).map(CString::into_raw).unwrap_or(std::ptr::null_mut())
}

fn item_json(item: &PyClipboardItem) -> String {
    serde_json::to_string(item).unwrap_or_else(|_| "null".to_string())
}

fn group_json(group: &PyGroup) -> serde_json::Value {
    serde_json::json!({
        "id": group.id,
        "name": group.name,
        "color": group.color,
        "icon": group.icon,
        "item_order": group.item_order,
        "created_at": group.created_at,
        "group_type": group.group_type,
    })
}

unsafe fn opt_i64(ptr: *const i64) -> Option<i64> {
    ptr.as_ref().copied()
}

unsafe fn opt_string(ptr: *const c_char) -> Option<String> {
    str_arg(ptr).map(str::to_owned)
}

unsafe fn put_text(text: &str, out: *mut *mut c_char) {
    if let Some(slot) = out.as_mut() {
        *slot = owned_text(text);
    }
}

/// Reports a result: Ok stores the value through `store`, Err writes the message.
unsafe fn finish<T>(result: Result<T, String>, error: *mut c_char, error_len: usize, store: impl FnOnce(T)) -> bool {
    match result {
        Ok(value) => {
            store(value);
            true
        }
        Err(message) => {
            write_text(&message, error, error_len);
            false
        }
    }
}

macro_rules! manager_or_fail {
    ($ptr:expr, $error:expr, $error_len:expr) => {
        match $ptr.as_ref() {
            Some(clipboard) => &clipboard.inner,
            None => {
                write_text("invalid argument", $error, $error_len);
                return false;
            }
        }
    };
}

/// Opens (creating when needed) the database at `db_path`. Null on failure.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_open(db_path: *const c_char, error: *mut c_char, error_len: usize) -> *mut JbClipboard {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        let Some(path) = str_arg(db_path) else {
            write_text("invalid database path", error, error_len);
            return std::ptr::null_mut();
        };
        match ClipboardManager::new(Some(path.to_owned())) {
            Ok(inner) => Box::into_raw(Box::new(JbClipboard { inner })),
            Err(message) => {
                write_text(&message, error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_free(ptr: *mut JbClipboard) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    })
}

/// Frees text returned by this module.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_text_free(ptr: *mut c_char) {
    guard((), || {
        if !ptr.is_null() {
            drop(CString::from_raw(ptr));
        }
    })
}

/// Frees bytes returned by jb_clipboard_get_image_data.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_bytes_free(ptr: *mut u8, len: usize) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
        }
    })
}

/// Frees ids returned by jb_clipboard_paste_items.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_ids_free(ptr: *mut i64, len: usize) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
        }
    })
}

#[no_mangle]
pub extern "C" fn jb_clipboard_max_stitched_pixels() -> u64 {
    guard(0, || {
        multi_paste::MAX_STITCHED_PIXELS
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_set_text(text: *const c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let Some(text) = str_arg(text) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        finish(manager::set_clipboard_text(text), error, error_len, |_| {})
    })
}

/// The database path; free with jb_clipboard_text_free.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_db_path(ptr: *const JbClipboard) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        ptr.as_ref().map_or(std::ptr::null_mut(), |clipboard| owned_text(&clipboard.inner.db_path()))
    })
}

/// The image folder beside the database (created when missing); free with jb_clipboard_text_free.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_images_dir(ptr: *const JbClipboard) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        ptr.as_ref().map_or(std::ptr::null_mut(), |clipboard| owned_text(&clipboard.inner.get_images_dir()))
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_set_history_limit(ptr: *const JbClipboard, limit: i64) {
    guard((), || {
        if let Some(clipboard) = ptr.as_ref() {
            clipboard.inner.set_history_limit(limit);
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_history_limit(ptr: *const JbClipboard) -> i64 {
    guard(0, || {
        ptr.as_ref().map_or(0, |clipboard| clipboard.inner.get_history_limit())
    })
}

/// No lock to take before the item callback, unlike the Python binding's GIL.
fn run_callback(call: &mut dyn FnMut()) {
    call();
}

/// Starts the watcher thread; a null callback records without notifying.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_start_monitor(
    ptr: *const JbClipboard,
    callback: JbClipboardItemCallback,
    user: *mut c_void,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let callback = callback.map(|callback| {
            let target = CallbackTarget { callback, user };
            Arc::new(move |item: &PyClipboardItem| target.notify(item)) as manager::ItemCallback
        });
        finish(manager.start_monitor(callback, run_callback), error, error_len, |_| {})
    })
}

/// Stops the watcher and waits for its thread; the callback is not called afterwards.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_stop_monitor(ptr: *const JbClipboard) {
    guard((), || {
        if let Some(clipboard) = ptr.as_ref() {
            clipboard.inner.stop_monitor(|join| join());
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_is_monitoring(ptr: *const JbClipboard) -> bool {
    guard(false, || {
        ptr.as_ref().is_some_and(|clipboard| clipboard.inner.is_monitoring())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_pause_for_update(ptr: *const JbClipboard) -> bool {
    guard(false, || {
        ptr.as_ref().is_some_and(|clipboard| clipboard.inner.pause_for_update())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_resume_after_update(ptr: *const JbClipboard) {
    guard((), || {
        if let Some(clipboard) = ptr.as_ref() {
            clipboard.inner.resume_after_update();
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_pending_writes(ptr: *const JbClipboard) -> usize {
    guard(0, || {
        ptr.as_ref().map_or(0, |clipboard| clipboard.inner.pending_writes())
    })
}

/// One page of history as a JSON array of items.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_clipboard_get_history(
    ptr: *const JbClipboard,
    offset: i64,
    limit: i64,
    search: *const c_char,
    content_type: *const c_char,
    start_time: *const i64,
    end_time: *const i64,
    out_json: *mut *mut c_char,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let result = manager.get_history(offset, limit, opt_string(search), opt_string(content_type), opt_i64(start_time), opt_i64(end_time));
        finish(result, error, error_len, |page| {
            put_text(&serde_json::to_string(&page.items).unwrap_or_else(|_| "[]".to_string()), out_json)
        })
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_get_count(ptr: *const JbClipboard, out: *mut i64, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.get_count(), error, error_len, |count| {
            if let Some(slot) = out.as_mut() {
                *slot = count;
            }
        })
    })
}

/// The item as JSON, or "null" when no such id.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_get_item(ptr: *const JbClipboard, id: i64, out_json: *mut *mut c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.get_item(id), error, error_len, |item| {
            put_text(&item.as_ref().map_or_else(|| "null".to_string(), item_json), out_json)
        })
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_delete_item(ptr: *const JbClipboard, id: i64, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.delete_item(id), error, error_len, |_| {})
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_clear_history(ptr: *const JbClipboard, keep_grouped: bool, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.clear_history(keep_grouped), error, error_len, |_| {})
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_toggle_pin(ptr: *const JbClipboard, id: i64, out_pinned: *mut bool, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.toggle_pin(id), error, error_len, |pinned| {
            if let Some(slot) = out_pinned.as_mut() {
                *slot = pinned;
            }
        })
    })
}

/// Adds an item; null content_type means "text", null title means none.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_add_item(
    ptr: *const JbClipboard,
    content: *const c_char,
    content_type: *const c_char,
    title: *const c_char,
    out_id: *mut i64,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let Some(content) = opt_string(content) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        finish(manager.add_item(content, opt_string(content_type), opt_string(title)), error, error_len, |id| {
            if let Some(slot) = out_id.as_mut() {
                *slot = id;
            }
        })
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_update_item(
    ptr: *const JbClipboard,
    id: i64,
    content: *const c_char,
    title: *const c_char,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let Some(content) = opt_string(content) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        finish(manager.update_item(id, content, opt_string(title)), error, error_len, |_| {})
    })
}

/// Null before/after ids mean the start/end of the list.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_move_item_between(
    ptr: *const JbClipboard,
    id: i64,
    before_id: *const i64,
    after_id: *const i64,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.move_item_between(id, opt_i64(before_id), opt_i64(after_id)), error, error_len, |_| {})
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_create_group(
    ptr: *const JbClipboard,
    name: *const c_char,
    color: *const c_char,
    icon: *const c_char,
    group_type: i64,
    out_id: *mut i64,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let Some(name) = opt_string(name) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        finish(manager.create_group(name, opt_string(color), opt_string(icon), group_type), error, error_len, |id| {
            if let Some(slot) = out_id.as_mut() {
                *slot = id;
            }
        })
    })
}

/// Every group as a JSON array.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_get_groups(ptr: *const JbClipboard, out_json: *mut *mut c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.get_groups(), error, error_len, |groups| {
            let list: Vec<serde_json::Value> = groups.iter().map(group_json).collect();
            put_text(&serde_json::Value::Array(list).to_string(), out_json)
        })
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_delete_group(ptr: *const JbClipboard, id: i64, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.delete_group(id), error, error_len, |_| {})
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_rename_group(ptr: *const JbClipboard, id: i64, name: *const c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let Some(name) = opt_string(name) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        finish(manager.rename_group(id, name), error, error_len, |_| {})
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_update_group(
    ptr: *const JbClipboard,
    id: i64,
    name: *const c_char,
    color: *const c_char,
    icon: *const c_char,
    group_type: i64,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let Some(name) = opt_string(name) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        finish(manager.update_group(id, name, opt_string(color), opt_string(icon), group_type), error, error_len, |_| {})
    })
}

/// A null group id takes the item out of its group.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_move_to_group(ptr: *const JbClipboard, item_id: i64, group_id: *const i64, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.move_to_group(item_id, opt_i64(group_id)), error, error_len, |_| {})
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_move_group_between(
    ptr: *const JbClipboard,
    id: i64,
    before_id: *const i64,
    after_id: *const i64,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.move_group_between(id, opt_i64(before_id), opt_i64(after_id)), error, error_len, |_| {})
    })
}

/// One page of a group (null id: ungrouped items) as a JSON array of items.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_get_by_group(
    ptr: *const JbClipboard,
    group_id: *const i64,
    offset: i64,
    limit: i64,
    out_json: *mut *mut c_char,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.get_by_group(opt_i64(group_id), offset, limit), error, error_len, |page| {
            put_text(&serde_json::to_string(&page.items).unwrap_or_else(|_| "[]".to_string()), out_json)
        })
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_paste_item(
    ptr: *const JbClipboard,
    id: i64,
    with_html: bool,
    move_to_top: bool,
    out_ok: *mut bool,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        finish(manager.paste_item(id, with_html, move_to_top), error, error_len, |ok| {
            if let Some(slot) = out_ok.as_mut() {
                *slot = ok;
            }
        })
    })
}

/// Pastes several items as one; `*out_ids` (free with jb_clipboard_ids_free) holds the ids used.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_clipboard_paste_items(
    ptr: *const JbClipboard,
    ids: *const i64,
    count: usize,
    with_html: bool,
    move_to_top: bool,
    separator: *const c_char,
    plain_text: bool,
    layout: *const c_char,
    keep_in_history: bool,
    out_ids: *mut *mut i64,
    out_count: *mut usize,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let ids = if count == 0 || ids.is_null() { Vec::new() } else { std::slice::from_raw_parts(ids, count).to_vec() };
        let result = MultiPasteOptions::new(with_html, move_to_top, opt_string(separator), plain_text, opt_string(layout).as_deref(), keep_in_history)
            .and_then(|options| manager.paste_items(&ids, &options));
        finish(result, error, error_len, |used| {
            let mut boxed = used.into_boxed_slice();
            if let Some(len) = out_count.as_mut() {
                *len = boxed.len();
            }
            if let Some(slot) = out_ids.as_mut() {
                *slot = boxed.as_mut_ptr();
                std::mem::forget(boxed);
            }
        })
    })
}

/// The stored PNG of an image item; `*out` stays null when the file is missing.
#[no_mangle]
pub unsafe extern "C" fn jb_clipboard_get_image_data(
    ptr: *const JbClipboard,
    image_id: *const c_char,
    out: *mut *mut u8,
    out_len: *mut usize,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let manager = manager_or_fail!(ptr, error, error_len);
        let Some(image_id) = str_arg(image_id) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        if let Some(slot) = out.as_mut() {
            *slot = std::ptr::null_mut();
        }
        if let Some(len) = out_len.as_mut() {
            *len = 0;
        }
        finish(manager.get_image_data(image_id), error, error_len, |data| {
            if let Some(bytes) = data {
                let mut boxed = bytes.into_boxed_slice();
                if let Some(len) = out_len.as_mut() {
                    *len = boxed.len();
                }
                if let Some(slot) = out.as_mut() {
                    *slot = boxed.as_mut_ptr();
                    std::mem::forget(boxed);
                }
            }
        })
    })
}
