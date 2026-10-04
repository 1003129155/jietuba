//! Remove only flat, updater-owned UUID directories. Installation journals live elsewhere.
use crate::{platform, safe_join, valid_id};
use std::{fs, path::Path};

pub fn remove(root: &Path, id: &str) {
    let result = (|| -> crate::Result<()> {
        valid_id(id)?;
        let dir = safe_join(root, id)?;
        let mut files = fs::read_dir(&dir)?
            .map(|entry| entry.map(|entry| entry.path()))
            .collect::<std::io::Result<Vec<_>>>()?;
        for file in &files {
            let meta = fs::symlink_metadata(file)?;
            if !meta.is_file() || meta.file_type().is_symlink() || platform::is_reparse(&meta) {
                return crate::fail("path", "缓存包含非普通文件，跳过清理");
            }
        }
        files.sort_by_key(|file| {
            file.file_name()
                .is_none_or(|name| name != "jietuba_updater.exe")
        });
        for file in files {
            fs::remove_file(file)?;
        }
        fs::remove_dir(dir)?;
        Ok(())
    })();
    // Cleanup must never turn a successful update into an error.
    let _ = result;
}

pub struct DownloadDirectory<'a> {
    pub root: &'a Path,
    pub id: &'a str,
    pub keep: bool,
}

impl Drop for DownloadDirectory<'_> {
    fn drop(&mut self) {
        if !self.keep {
            remove(self.root, self.id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn failed_download_is_removed_but_success_is_retained() {
        let root = std::env::temp_dir().join(crate::random_id());
        let id = crate::random_id();
        let dir = root.join(&id);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("package.zip"), b"partial download").unwrap();
        {
            let _guard = DownloadDirectory {
                root: &root,
                id: &id,
                keep: false,
            };
        }
        assert!(!dir.exists());
        fs::create_dir(&dir).unwrap();
        fs::write(dir.join("app.exe"), b"ready").unwrap();
        {
            let _guard = DownloadDirectory {
                root: &root,
                id: &id,
                keep: true,
            };
        }
        assert!(dir.join("app.exe").exists());
        remove(&root, &id);
        fs::remove_dir(root).unwrap();
    }

    #[test]
    fn cleanup_refuses_unexpected_subdirectories_and_invalid_ids() {
        let root = std::env::temp_dir().join(crate::random_id());
        let id = crate::random_id();
        let dir = root.join(&id);
        fs::create_dir_all(dir.join("unexpected")).unwrap();
        fs::write(dir.join("app.exe"), b"preserve").unwrap();
        remove(&root, &id);
        remove(&root, "../outside");
        assert!(dir.join("app.exe").exists());
        fs::remove_dir(dir.join("unexpected")).unwrap();
        remove(&root, &id);
        fs::remove_dir(root).unwrap();
    }
}
