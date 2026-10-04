use crate::{
    contract::{self, AppVariant, Arch, DownloadArtifact, Release},
    fail, Result,
};
use serde_json::Value;
use std::{
    fs,
    io::Write,
    path::Path,
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};

pub const LATEST: &str = "https://api.github.com/repos/1003129155/jietuba/releases/latest";
pub fn parse_release(data: &Value, arch: Arch, variant: AppVariant) -> Result<Release> {
    if data.get("draft").and_then(Value::as_bool) == Some(true)
        || data.get("prerelease").and_then(Value::as_bool) == Some(true)
    {
        return fail("release", "只支持公开稳定版本");
    }
    let tag = data
        .get("tag_name")
        .and_then(Value::as_str)
        .ok_or_else(|| crate::Error {
            code: "version",
            message: "Release 缺少版本号".into(),
        })?;
    contract::version(tag)?;
    let filename = variant.asset_name(tag, arch);
    let assets = data
        .get("assets")
        .and_then(Value::as_array)
        .ok_or_else(|| crate::Error {
            code: "asset",
            message: "Release 没有下载包".into(),
        })?;
    let matches: Vec<_> = assets
        .iter()
        .filter(|a| a.get("name").and_then(Value::as_str) == Some(&filename))
        .collect();
    if matches.len() != 1 {
        return fail("asset", "Release 没有唯一匹配架构的下载包");
    }
    let url = matches[0]
        .get("browser_download_url")
        .and_then(Value::as_str)
        .ok_or_else(|| crate::Error {
            code: "asset",
            message: "下载地址缺失".into(),
        })?;
    contract::validate_url(url)?;
    Ok(Release {
        tag_name: tag.into(),
        title: data
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or(tag)
            .chars()
            .take(500)
            .collect(),
        notes: data
            .get("body")
            .and_then(Value::as_str)
            .unwrap_or("")
            .chars()
            .take(12000)
            .collect(),
        url: format!("https://github.com/1003129155/jietuba/releases/tag/{tag}"),
        asset_name: filename,
        arch,
        artifact: DownloadArtifact {
            urls: vec![url.into()],
            size: matches[0].get("size").and_then(Value::as_u64),
        },
    })
}
pub fn client() -> Result<reqwest::Client> {
    reqwest::Client::builder()
        .user_agent("jietuba-updater")
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(900))
        .build()
        .map_err(network)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn selects_the_installed_variant_and_architecture() {
        let tag = "release-2.2.0";
        let assets: Vec<_> = [AppVariant::Full, AppVariant::Lite].into_iter()
            .flat_map(|variant| [Arch::X64, Arch::Arm64].into_iter().map(move |arch| json!({
                "name": variant.asset_name(tag, arch),
                "browser_download_url": format!("https://example.com/{}", variant.asset_name(tag, arch)),
                "size": 123,
            }))).collect();
        let payload = json!({"tag_name": tag, "assets": assets});
        for variant in [AppVariant::Full, AppVariant::Lite] {
            for arch in [Arch::X64, Arch::Arm64] {
                let release = parse_release(&payload, arch, variant).unwrap();
                assert_eq!(release.asset_name, variant.asset_name(tag, arch));
                assert_eq!(release.arch, arch);
            }
        }
    }

    #[test]
    fn missing_lite_package_does_not_fall_back_to_full() {
        let payload = json!({"tag_name": "release-2.2.0", "assets": [{
            "name": "jietuba_pp-release-2.2.0-x64.zip",
            "browser_download_url": "https://example.com/full.zip",
        }]});
        assert_eq!(
            parse_release(&payload, Arch::X64, AppVariant::Lite)
                .unwrap_err()
                .code,
            "asset"
        );
    }
}
fn network(e: reqwest::Error) -> crate::Error {
    crate::Error {
        code: "network",
        message: e.to_string(),
    }
}
pub async fn cancelled(flag: &AtomicBool) {
    loop {
        if flag.load(Ordering::Acquire) {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
}
pub async fn fetch_latest(arch: Arch, variant: AppVariant, flag: &AtomicBool) -> Result<Release> {
    let client = client()?;
    let response = tokio::select! {
        r = client.get(LATEST).header("Accept", "application/vnd.github+json").send() => r.map_err(network)?.error_for_status().map_err(network)?,
        _ = cancelled(flag) => return fail("cancelled", "检查已取消"),
    };
    let bytes = tokio::select! { r = response.bytes() => r.map_err(network)?, _ = cancelled(flag) => return fail("cancelled", "检查已取消") };
    if bytes.len() > 1024 * 1024 {
        return fail("release", "Release 响应过大");
    }
    parse_release(&serde_json::from_slice(&bytes)?, arch, variant)
}
pub async fn download_file(
    artifact: &DownloadArtifact,
    destination: &Path,
    flag: &AtomicBool,
    mut progress: impl FnMut(u64, Option<u64>, &str) -> Result<()>,
) -> Result<()> {
    if artifact.urls.is_empty() {
        return fail("source", "没有下载源");
    }
    let client = client()?;
    let partial = destination.with_extension("part");
    let mut last_error = crate::Error {
        code: "network",
        message: "下载失败".into(),
    };
    for url in &artifact.urls {
        contract::validate_url(url)?;
        for attempt in 0..2 {
            let result = async {
                if flag.load(Ordering::Acquire) { return fail("cancelled", "下载已取消"); }
                let mut response = tokio::select! { r = client.get(url).send() => r.map_err(network)?.error_for_status().map_err(network)?, _ = cancelled(flag) => return fail("cancelled", "下载已取消") };
                let total = response.content_length().or(artifact.size);
                if total.is_some_and(|n| n > 2 * 1024 * 1024 * 1024) { return fail("size", "下载包过大"); }
                let mut output = fs::OpenOptions::new().write(true).create_new(true).open(&partial)?;
                let mut received = 0; let mut last = Instant::now(); progress(0, total, url)?;
                loop {
                    let chunk = tokio::select! { r = response.chunk() => r.map_err(network)?, _ = cancelled(flag) => return fail("cancelled", "下载已取消") };
                    let Some(chunk) = chunk else { break; };
                    received += chunk.len() as u64;
                    if received > 2 * 1024 * 1024 * 1024 { return fail("size", "下载包过大"); }
                    output.write_all(&chunk)?;
                    if last.elapsed() >= Duration::from_millis(100) { progress(received, total, url)?; last = Instant::now(); }
                }
                output.sync_all()?; drop(output);
                if total.is_some_and(|n| n != received) { return fail("network", "下载内容不完整"); }
                progress(received, total, url)?;
                crate::platform::durable_rename(&partial, destination)
            }.await;
            match result {
                Ok(()) => return Ok(()),
                Err(e) => {
                    let _ = fs::remove_file(&partial);
                    if e.code == "cancelled" || e.code == "protocol" {
                        return Err(e);
                    }
                    last_error = e;
                }
            }
            if attempt == 0 {
                tokio::select! { _ = tokio::time::sleep(Duration::from_millis(250)) => {}, _ = cancelled(flag) => return fail("cancelled", "下载已取消") };
            }
        }
    }
    Err(last_error)
}
