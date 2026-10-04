use crate::{
    contract::{AppVariant, Arch},
    fail, platform, Result,
};
use std::{
    fs,
    io::{self, Read},
    path::Path,
};
pub fn extract_exe(
    zip_path: &Path,
    destination: &Path,
    arch: Arch,
    variant: AppVariant,
) -> Result<()> {
    let filename = variant.executable_name();
    let mut archive =
        zip::ZipArchive::new(fs::File::open(zip_path)?).map_err(|e| crate::Error {
            code: "archive",
            message: e.to_string(),
        })?;
    let matches: Vec<_> = (0..archive.len())
        .filter(|&i| {
            archive
                .by_index(i)
                .is_ok_and(|entry| entry.name().eq_ignore_ascii_case(&filename))
        })
        .collect();
    if matches.len() != 1 {
        return fail("archive", format!("ZIP 根目录必须包含唯一的 {filename}"));
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn extracts_only_the_selected_executable() {
        let dir = std::env::temp_dir().join(format!("jietuba-updater-test-{}", crate::random_id()));
        fs::create_dir(&dir).unwrap();
        let package = dir.join("package.zip");
        let staged = dir.join("staged.exe");
        let mut zip = zip::ZipWriter::new(fs::File::create(&package).unwrap());
        for (variant, marker) in [(AppVariant::Full, 1u8), (AppVariant::Lite, 2u8)] {
            let mut pe = vec![0u8; 256];
            pe[..2].copy_from_slice(b"MZ");
            pe[60..64].copy_from_slice(&64u32.to_le_bytes());
            pe[64..68].copy_from_slice(b"PE\0\0");
            pe[68..70].copy_from_slice(&0x8664u16.to_le_bytes());
            pe[70..72].copy_from_slice(&1u16.to_le_bytes());
            pe[84..86].copy_from_slice(&112u16.to_le_bytes());
            pe[86..88].copy_from_slice(&2u16.to_le_bytes());
            pe[88..90].copy_from_slice(&0x20bu16.to_le_bytes());
            pe[255] = marker;
            zip.start_file(
                variant.executable_name(),
                zip::write::SimpleFileOptions::default(),
            )
            .unwrap();
            zip.write_all(&pe).unwrap();
        }
        zip.finish().unwrap();
        for (variant, marker) in [(AppVariant::Full, 1u8), (AppVariant::Lite, 2u8)] {
            extract_exe(&package, &staged, Arch::X64, variant).unwrap();
            assert_eq!(fs::read(&staged).unwrap()[255], marker);
            fs::remove_file(&staged).unwrap();
        }
        fs::remove_file(package).unwrap();
        fs::remove_dir(dir).unwrap();
    }
}
