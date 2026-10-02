use crate::{fail, hash_file, platform, safe_join, valid_id, write_json, Result};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Journal {
    pub transaction: String,
    pub executable: PathBuf,
    pub phase: String,
    pub old_hash: String,
    pub new_hash: String,
}
pub fn install_exe(path: &Path) -> Result<PathBuf> {
    let parent = path.parent().ok_or_else(|| crate::Error {
        code: "path",
        message: "主程序路径必须为绝对路径".into(),
    })?;
    if !path.is_absolute() {
        return fail("path", "主程序路径必须为绝对路径");
    }
    let root = fs::canonicalize(parent)?;
    let filename = path.file_name().ok_or_else(|| crate::Error {
        code: "path",
        message: "主程序路径无效".into(),
    })?;
    safe_join(&root, &filename.to_string_lossy())
}
pub fn root(executable: &Path) -> Result<PathBuf> {
    let path = safe_join(executable.parent().unwrap(), ".jietuba-update")?;
    fs::create_dir_all(&path)?;
    Ok(path)
}
pub fn lock(executable: &Path) -> Result<fs::File> {
    let file = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(safe_join(&root(executable)?, "lock")?)?;
    file.try_lock_exclusive().map_err(|_| crate::Error {
        code: "busy",
        message: "已有更新或恢复正在进行".into(),
    })?;
    Ok(file)
}
fn durable_copy(source: &Path, dest: &Path) -> Result<()> {
    use std::io;
    let mut input = fs::File::open(source)?;
    let mut output = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(dest)?;
    io::copy(&mut input, &mut output)?;
    output.sync_all()?;
    Ok(())
}
pub fn pending(executable: &Path) -> Result<()> {
    for entry in fs::read_dir(root(executable)?)? {
        let entry = entry?;
        let id = entry.file_name().to_string_lossy().into_owned();
        if valid_id(&id).is_err() {
            continue;
        }
        let dir = safe_join(&root(executable)?, &id)?;
        let record = safe_join(&dir, "journal.json")?;
        if record.exists() {
            let j: Journal = serde_json::from_slice(&fs::read(record)?)?;
            if j.executable != executable || !["complete", "recovered"].contains(&j.phase.as_str())
            {
                return fail("pending_recovery", format!("请先恢复未完成事务 {id}"));
            }
        }
    }
    Ok(())
}
pub fn prepare(executable: &Path, staged: &Path, id: &str) -> Result<Journal> {
    valid_id(id)?;
    pending(executable)?;
    if platform::pe_arch(executable)? != platform::pe_arch(staged)? {
        return fail("architecture", "主程序架构不匹配");
    }
    let dir = safe_join(&root(executable)?, id)?;
    fs::create_dir(&dir)?;
    let mut journal = Journal {
        transaction: id.into(),
        executable: executable.into(),
        phase: "preparing".into(),
        old_hash: hash_file(executable)?,
        new_hash: hash_file(staged)?,
    };
    save(&journal)?;
    durable_copy(executable, &safe_join(&dir, "backup.exe")?)?;
    durable_copy(staged, &safe_join(&dir, "new.exe")?)?;
    journal.phase = "prepared".into();
    save(&journal)?;
    Ok(journal)
}
pub fn directory(j: &Journal) -> Result<PathBuf> {
    valid_id(&j.transaction)?;
    safe_join(&root(&j.executable)?, &j.transaction)
}
pub fn save(j: &Journal) -> Result<()> {
    write_json(&safe_join(&directory(j)?, "journal.json")?, j)
}
pub fn replace(j: &mut Journal) -> Result<()> {
    let dir = directory(j)?;
    if hash_file(&j.executable)? != j.old_hash
        || hash_file(&safe_join(&dir, "backup.exe")?)? != j.old_hash
        || hash_file(&safe_join(&dir, "new.exe")?)? != j.new_hash
    {
        return fail("conflict", "主程序或事务内容已改变");
    }
    j.phase = "replacing".into();
    save(j)?;
    platform::durable_rename(&safe_join(&dir, "new.exe")?, &j.executable)?;
    j.phase = "replaced".into();
    save(j)
}
pub fn recover(executable: &Path, id: &str) -> Result<()> {
    valid_id(id)?;
    let dir = safe_join(&root(executable)?, id)?;
    let mut j: Journal = serde_json::from_slice(&fs::read(safe_join(&dir, "journal.json")?)?)?;
    if j.executable != executable || j.transaction != id {
        return fail("path", "恢复事务不属于当前主程序");
    }
    let current = if executable.exists() {
        Some(hash_file(executable)?)
    } else {
        None
    };
    if current.as_ref() == Some(&j.old_hash) {
        j.phase = "recovered".into();
        return save(&j);
    }
    if current.is_some() && current.as_ref() != Some(&j.new_hash) {
        return fail("conflict", "主程序被外部修改，保留备份供手动恢复");
    }
    let backup = safe_join(&dir, "backup.exe")?;
    if hash_file(&backup)? != j.old_hash {
        return fail("recovery_failed", "备份损坏，保留事务记录");
    }
    let temp = safe_join(&dir, &format!("restore-{}.exe", crate::random_id()))?;
    durable_copy(&backup, &temp)?;
    platform::durable_rename(&temp, executable)?;
    j.phase = "recovered".into();
    save(&j)
}
pub fn complete(j: &mut Journal) -> Result<()> {
    j.phase = "complete".into();
    save(j)?;
    // Remove only known files from older updater transactions and keep the latest backup.
    for entry in fs::read_dir(root(&j.executable)?)? {
        let entry = entry?;
        let id = entry.file_name().to_string_lossy().into_owned();
        if id == j.transaction || valid_id(&id).is_err() {
            continue;
        }
        let dir = safe_join(&root(&j.executable)?, &id)?;
        let record = safe_join(&dir, "journal.json")?;
        if let Ok(bytes) = fs::read(&record) {
            if let Ok(old) = serde_json::from_slice::<Journal>(&bytes) {
                if old.executable == j.executable
                    && ["complete", "recovered"].contains(&old.phase.as_str())
                {
                    for name in ["backup.exe", "new.exe", "journal.json"] {
                        let _ = fs::remove_file(safe_join(&dir, name)?);
                    }
                    let _ = fs::remove_dir(dir);
                }
            }
        }
    }
    Ok(())
}
