//! 模型文件下载：把清单条目里的文件逐个下载到 `models/.download-<staging>/`，按大小和 SHA-256 校验。
//! 写 model.json、改名就位由主程序完成，这里只保证临时目录里的文件和清单一致。
//! 文件也可以取自一个 ZIP（例如官方 wheel 里的 onnxruntime.dll）：先校验整个压缩包，再取出
//! 那一个成员并校验它，压缩包随后删掉。

use crate::{contract::DownloadArtifact, download, fail, hash_file, safe_join, Result};
use serde::Deserialize;
use std::{
    fs,
    io::{self, Read},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
};

/// 单个文件的大小上限，和应用更新包一致。
const MAX_FILE: u64 = 2 * 1024 * 1024 * 1024;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ModelRequest {
    /// 清单里的模型 id，或运行库的 `runtime`。
    pub staging: String,
    pub files: Vec<ModelFile>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ModelFile {
    pub name: String,
    pub size: u64,
    pub sha256: String,
    pub urls: Vec<String>,
    /// 有它时 urls 下载的是这个压缩包，文件取自其中的 member。
    #[serde(default)]
    pub archive: Option<ModelArchive>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ModelArchive {
    pub size: u64,
    pub sha256: String,
    pub member: String,
}

fn staging_ok(name: &str) -> bool {
    (1..=64).contains(&name.len()) && name.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

fn file_name_ok(name: &str) -> bool {
    (1..=128).contains(&name.len())
        && !name.starts_with('.')
        && name.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
}

pub fn validate(request: &ModelRequest) -> Result<()> {
    if !staging_ok(&request.staging) || request.files.is_empty() {
        return fail("arguments", "模型下载请求无效");
    }
    for (i, file) in request.files.iter().enumerate() {
        let duplicate = request.files[..i].iter().any(|f| f.name.eq_ignore_ascii_case(&file.name));
        let archive_ok = file.archive.as_ref().is_none_or(|a| {
            hex_ok(&a.sha256) && a.size > 0 && a.size <= MAX_FILE && member_ok(&a.member)
        });
        if !file_name_ok(&file.name) || duplicate || !hex_ok(&file.sha256) || file.size == 0 || file.size > MAX_FILE || file.urls.is_empty() || !archive_ok {
            return fail("arguments", format!("模型文件描述无效: {}", file.name));
        }
        for url in &file.urls {
            crate::contract::validate_url(url)?;
        }
    }
    Ok(())
}

fn hex_ok(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// 压缩包里的相对路径：用 `/` 分隔，没有空段、`.`、`..`、盘符和反斜杠。
fn member_ok(member: &str) -> bool {
    (1..=256).contains(&member.len())
        && member.split('/').all(|part| !part.is_empty() && part != "." && part != ".." && !part.contains(['\\', ':']))
}

/// 下载完的文件必须和清单的大小、哈希都对得上。
pub fn verify(path: &Path, file: &ModelFile) -> Result<()> {
    if fs::metadata(path)?.len() != file.size || hash_file(path)? != file.sha256 {
        return fail("hash", format!("下载的文件校验失败: {}", file.name));
    }
    Ok(())
}

/// 从已校验的压缩包里取出 member 写到 target，大小必须正好是 size。
pub fn extract_member(archive: &Path, member: &str, target: &Path, size: u64) -> Result<()> {
    let invalid = |message: String| crate::Error { code: "archive", message };
    let mut zip = zip::ZipArchive::new(fs::File::open(archive)?).map_err(|e| invalid(e.to_string()))?;
    let mut entry = zip.by_name(member).map_err(|_| invalid(format!("压缩包里没有 {member}")))?;
    if entry.size() != size || entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000) {
        return fail("archive", format!("压缩包里的 {member} 无效"));
    }
    let mut output = fs::OpenOptions::new().write(true).create_new(true).open(target)?;
    let written = io::copy(&mut (&mut entry).take(size + 1), &mut output)?;
    output.sync_all()?;
    if written != size {
        return fail("archive", format!("压缩包里的 {member} 不完整"));
    }
    Ok(())
}

/// 下载请求里的全部文件，返回临时目录。失败或取消时删掉临时目录。
/// `progress` 收到的是全部文件合计的已下载字节数和总字节数。
pub async fn download(
    request: &ModelRequest,
    models_root: &Path,
    flag: &AtomicBool,
    mut progress: impl FnMut(u64, u64, &str) -> Result<()>,
) -> Result<PathBuf> {
    validate(request)?;
    fs::create_dir_all(models_root)?;
    let staging = safe_join(models_root, &format!(".download-{}", request.staging))?;
    if staging.exists() {
        fs::remove_dir_all(&staging)?;
    }
    fs::create_dir(&staging)?;
    let download_size = |f: &ModelFile| f.archive.as_ref().map_or(f.size, |a| a.size);
    let total: u64 = request.files.iter().map(download_size).sum();
    let result = async {
        let mut done = 0;
        for file in &request.files {
            let target = safe_join(&staging, &file.name)?;
            let downloaded = match &file.archive {
                Some(_) => safe_join(&staging, &format!("{}.zip", file.name))?,
                None => target.clone(),
            };
            let artifact = DownloadArtifact { urls: file.urls.clone(), size: Some(download_size(file)) };
            download::download_file(&artifact, &downloaded, flag, |received, _, source| progress(done + received, total, source)).await?;
            if flag.load(Ordering::Acquire) {
                return fail("cancelled", "下载已取消");
            }
            if let Some(archive) = &file.archive {
                if fs::metadata(&downloaded)?.len() != archive.size || hash_file(&downloaded)? != archive.sha256 {
                    return fail("hash", format!("下载的压缩包校验失败: {}", file.name));
                }
                extract_member(&downloaded, &archive.member, &target, file.size)?;
                fs::remove_file(&downloaded)?;
            }
            verify(&target, file)?;
            done += download_size(file);
        }
        Ok(())
    }
    .await;
    match result {
        Ok(()) => Ok(staging),
        Err(e) => {
            let _ = fs::remove_dir_all(&staging);
            Err(e)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file(name: &str) -> ModelFile {
        ModelFile {
            name: name.into(),
            size: 3,
            sha256: "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad".into(),
            urls: vec!["https://example.com/a".into()],
            archive: None,
        }
    }

    fn archived(member: &str) -> ModelFile {
        ModelFile { archive: Some(ModelArchive { size: 10, sha256: "0".repeat(64), member: member.into() }), ..file("onnxruntime.dll") }
    }

    #[test]
    fn rejects_unsafe_names_and_sources() {
        let ok = ModelRequest { staging: "ppocr-v5-korean".into(), files: vec![file("korean_PP-OCRv5_rec_mobile.onnx")] };
        validate(&ok).unwrap();
        for staging in ["", "../x", "A", "x/y", ".download"] {
            assert!(validate(&ModelRequest { staging: staging.into(), files: vec![file("a.onnx")] }).is_err(), "{staging}");
        }
        for name in ["", "..", ".hidden", "a/b.onnx", "a\\b.onnx", "c:x", "模型.onnx"] {
            assert!(validate(&ModelRequest { staging: "x".into(), files: vec![file(name)] }).is_err(), "{name}");
        }
        assert!(validate(&ModelRequest { staging: "x".into(), files: vec![file("a.onnx"), file("A.onnx")] }).is_err());
        let mut http = file("a.onnx");
        http.urls = vec!["http://example.com/a".into()];
        assert!(validate(&ModelRequest { staging: "x".into(), files: vec![http] }).is_err());
        let mut upper = file("a.onnx");
        upper.sha256 = upper.sha256.to_uppercase();
        assert!(validate(&ModelRequest { staging: "x".into(), files: vec![upper] }).is_err());
        assert!(validate(&ModelRequest { staging: "x".into(), files: vec![] }).is_err());
        validate(&ModelRequest { staging: "runtime".into(), files: vec![archived("onnxruntime/capi/onnxruntime.dll")] }).unwrap();
        for member in ["", "../a.dll", "a//b.dll", "./a.dll", "c:/a.dll", "a\\b.dll", "/a.dll"] {
            assert!(validate(&ModelRequest { staging: "runtime".into(), files: vec![archived(member)] }).is_err(), "{member}");
        }
    }

    #[test]
    fn extract_member_takes_one_exact_entry() {
        use std::io::Write;
        let dir = std::env::temp_dir().join(format!("updater-archive-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let archive = dir.join("pkg.whl");
        let mut zip = zip::ZipWriter::new(fs::File::create(&archive).unwrap());
        let options = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        zip.start_file("onnxruntime/capi/onnxruntime.dll", options).unwrap();
        zip.write_all(b"abc").unwrap();
        zip.start_file("onnxruntime/capi/other.dll", options).unwrap();
        zip.write_all(b"other").unwrap();
        zip.finish().unwrap();

        extract_member(&archive, "onnxruntime/capi/onnxruntime.dll", &dir.join("onnxruntime.dll"), 3).unwrap();
        verify(&dir.join("onnxruntime.dll"), &file("onnxruntime.dll")).unwrap();
        assert!(!dir.join("other.dll").exists());
        assert_eq!(extract_member(&archive, "missing.dll", &dir.join("m.dll"), 3).unwrap_err().code, "archive");
        assert_eq!(extract_member(&archive, "onnxruntime/capi/other.dll", &dir.join("o.dll"), 3).unwrap_err().code, "archive");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn verify_checks_size_and_hash() {
        let dir = std::env::temp_dir().join(format!("updater-models-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("a.onnx");
        fs::write(&path, b"abc").unwrap();
        verify(&path, &file("a.onnx")).unwrap();
        fs::write(&path, b"abd").unwrap();
        assert_eq!(verify(&path, &file("a.onnx")).unwrap_err().code, "hash");
        fs::write(&path, b"abcd").unwrap();
        assert_eq!(verify(&path, &file("a.onnx")).unwrap_err().code, "hash");
        let _ = fs::remove_dir_all(&dir);
    }
}
