use crate::{contract::Arch, fail, platform, Result};
use std::{
    fs,
    io::{self, Read},
    path::Path,
};
pub fn extract_exe(zip_path: &Path, destination: &Path, arch: Arch) -> Result<()> {
    let mut archive =
        zip::ZipArchive::new(fs::File::open(zip_path)?).map_err(|e| crate::Error {
            code: "archive",
            message: e.to_string(),
        })?;
    let matches: Vec<_> = (0..archive.len())
        .filter(|&i| {
            archive
                .by_index(i)
                .is_ok_and(|entry| entry.name().eq_ignore_ascii_case("jietuba_pp.exe"))
        })
        .collect();
    if matches.len() != 1 {
        return fail("archive", "ZIP 根目录必须包含唯一的 jietuba_pp.exe");
    }
    let mut entry = archive.by_index(matches[0]).map_err(|e| crate::Error {
        code: "archive",
        message: e.to_string(),
    })?;
    if entry.size() == 0
        || entry.size() > 2 * 1024 * 1024 * 1024
        || entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000)
    {
        return fail("archive", "目标 EXE 无效");
    }
    let result = (|| {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(destination)?;
        // Extract only the application EXE, leaving models and all other ZIP entries untouched.
        let received = io::copy(
            &mut (&mut entry).take(2 * 1024 * 1024 * 1024 + 1),
            &mut file,
        )?;
        file.sync_all()?;
        drop(file);
        if received != entry.size() {
            return fail("archive", "目标 EXE 内容不完整");
        }
        if platform::pe_arch(destination)? != arch {
            return fail("architecture", "新版 EXE 架构与已安装应用不匹配");
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(destination);
    }
    result
}
