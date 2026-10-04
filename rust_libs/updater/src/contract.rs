use crate::{fail, Result};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AppVariant {
    Full,
    Lite,
}
impl AppVariant {
    /// The application reports its own build type, so a renamed EXE still updates in place.
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "full" => Ok(Self::Full),
            "lite" => Ok(Self::Lite),
            _ => fail("variant", "版本类型无效，应为 full 或 lite"),
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Full => "jietuba_pp",
            Self::Lite => "jietuba_lite",
        }
    }
    pub fn executable_name(self) -> String {
        format!("{}.exe", self.name())
    }
    pub fn asset_name(self, tag: &str, arch: Arch) -> String {
        format!("{}-{tag}-{}.zip", self.name(), arch.name())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Arch {
    X64,
    Arm64,
}
impl Arch {
    pub fn name(self) -> &'static str {
        match self {
            Self::X64 => "x64",
            Self::Arm64 => "arm64",
        }
    }
}

pub fn version(value: &str) -> Result<[u64; 4]> {
    let start = value
        .find(|c: char| c.is_ascii_digit())
        .ok_or_else(|| crate::Error {
            code: "version",
            message: "版本号无效".into(),
        })?;
    let digits: String = value[start..]
        .chars()
        .take_while(|c| c.is_ascii_digit() || *c == '.')
        .collect();
    let mut out = [0; 4];
    let parts: Vec<_> = digits.split('.').collect();
    if parts.is_empty() || parts.len() > 4 {
        return fail("version", "版本号无效");
    }
    for (i, part) in parts.iter().enumerate() {
        out[i] = part.parse().map_err(|_| crate::Error {
            code: "version",
            message: "版本号无效".into(),
        })?;
    }
    Ok(out)
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DownloadArtifact {
    pub urls: Vec<String>,
    pub size: Option<u64>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ModelArtifact {
    pub model_id: String,
    pub version: String,
    pub format: String,
    pub artifact: DownloadArtifact,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Release {
    pub tag_name: String,
    pub title: String,
    pub notes: String,
    pub url: String,
    pub asset_name: String,
    pub arch: Arch,
    pub artifact: DownloadArtifact,
}
pub trait ReleaseSource {
    fn candidates(&self, release: &Release) -> Result<Vec<String>>;
}
pub struct GitHubSource;
impl ReleaseSource for GitHubSource {
    fn candidates(&self, release: &Release) -> Result<Vec<String>> {
        Ok(release.artifact.urls.clone())
    }
}
pub fn validate_url(url: &str) -> Result<()> {
    let parsed = reqwest::Url::parse(url).map_err(|_| crate::Error {
        code: "source",
        message: "下载地址无效".into(),
    })?;
    if parsed.scheme() != "https"
        || parsed.host_str().is_none()
        || !parsed.username().is_empty()
        || parsed.password().is_some()
    {
        return fail("source", "下载源必须使用 HTTPS");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn variant_comes_from_the_reported_build_type() {
        assert_eq!(AppVariant::parse("lite").unwrap(), AppVariant::Lite);
        assert_eq!(AppVariant::parse("full").unwrap(), AppVariant::Full);
        assert_eq!(AppVariant::parse("pp").unwrap_err().code, "variant");
    }
}
