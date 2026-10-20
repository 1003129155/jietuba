//! 剪贴板历史：监听、存储、写回剪贴板。

pub mod cf_html;
pub mod database;
pub mod group_kind;
pub mod manager;
pub mod multi_paste;
pub mod text_kind;
pub mod types;
#[cfg(target_os = "windows")]
pub mod win_clipboard;
