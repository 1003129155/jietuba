pub mod archive;
pub mod cache;
pub mod contract;
pub mod download;
pub mod platform;
pub mod protocol;
pub mod transaction;

use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};

#[derive(Debug, thiserror::Error)]
#[error("{code}: {message}")]
pub struct Error {
    pub code: &'static str,
    pub message: String,
}
pub type Result<T> = std::result::Result<T, Error>;
pub fn fail<T>(code: &'static str, message: impl Into<String>) -> Result<T> {
    Err(Error {
        code,
        message: message.into(),
    })
}
impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Self {
            code: match e.raw_os_error() {
                #[cfg(windows)]
                Some(32 | 33) => "file_in_use",
                _ if e.kind() == std::io::ErrorKind::PermissionDenied => "permission",
                _ => "io",
            },
            message: e.to_string(),
        }
    }
}
impl From<serde_json::Error> for Error {
    fn from(e: serde_json::Error) -> Self {
        Self {
            code: "protocol",
            message: e.to_string(),
        }
    }
}
pub fn random_id() -> String {
    use rand::RngCore;
    let mut bytes = [0u8; 16];
    rand::rngs::OsRng.fill_bytes(&mut bytes);
    hex::encode(bytes)
}
pub fn valid_id(id: &str) -> Result<()> {
    if id.len() != 32
        || !id
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return fail("path", "无效事务标识");
    }
    Ok(())
}
// Fingerprints protect local backup and recovery files; releases need no hash or manifest.
pub fn hash_file(path: &Path) -> Result<String> {
    use sha2::{Digest, Sha256};
    let mut file = fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut buf = [0u8; 65536];
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hash.update(&buf[..n]);
    }
    Ok(hex::encode(hash.finalize()))
}
pub fn safe_join(root: &Path, relative: &str) -> Result<PathBuf> {
    if relative.is_empty()
        || relative
            .split('/')
            .any(|p| p.is_empty() || p == "." || p == ".." || p.contains(['\\', ':']))
    {
        return fail("path", "无效内部路径");
    }
    let mut path = root.to_path_buf();
    for part in relative.split('/') {
        path.push(part);
        if let Ok(meta) = fs::symlink_metadata(&path) {
            if meta.file_type().is_symlink() || platform::is_reparse(&meta) {
                return fail("path", "更新目录不允许链接或重解析点");
            }
        }
    }
    Ok(path)
}
pub fn write_json(path: &Path, value: &impl serde::Serialize) -> Result<()> {
    use std::io::Write;
    let temp = path.with_extension(format!("{}.tmp", random_id()));
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp)?;
    file.write_all(&serde_json::to_vec(value)?)?;
    file.sync_all()?;
    drop(file);
    platform::durable_rename(&temp, path)
}
