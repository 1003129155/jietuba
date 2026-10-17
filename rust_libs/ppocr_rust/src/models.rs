//! exe 旁 `models/` 目录里的 PP-OCR 模型，以及内置的可下载清单。
//!
//! 根目录的 [`DEFAULT_DET`] + [`DEFAULT_REC`] 是默认模型（id 为空串），老版本的包和更新器都按这个
//! 布局放文件。其他模型各占一个子目录，目录名即 id，由其中的 [`MANIFEST`] 描述。名字以 `.` 开头的
//! 子目录是下载或添加途中的临时目录，不算模型。

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use serde::{Deserialize, Serialize};

use crate::engine::Engine;

pub const DEFAULT_DET: &str = "PP-OCRv6_det_small.onnx";
pub const DEFAULT_REC: &str = "PP-OCRv6_rec_small.onnx";
pub const MANIFEST: &str = "model.json";
pub const RUNTIME_DLL: &str = "onnxruntime.dll";
/// 运行时下载到的临时目录名，和模型的 `.download-<id>` 共用前缀。
pub const RUNTIME_STAGING: &str = ".download-runtime";
const STAGING_PREFIXES: [&str; 2] = [".download-", ".import-"];

/// 清单里的一个文件：下载后的文件名、大小、SHA-256（小写十六进制）和按顺序尝试的下载地址。
/// 有 archive 时地址给的是一个 ZIP，文件是其中的 member，两者都校验。
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct RemoteFile {
    pub name: String,
    pub size: u64,
    pub sha256: String,
    pub urls: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub archive: Option<RemoteArchive>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct RemoteArchive {
    pub size: u64,
    pub sha256: String,
    pub member: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogModel {
    pub id: String,
    pub name: String,
    /// 界面按这些代码显示语种：zh、ja、en、ko、latin、cyrillic、th、el。
    pub languages: Vec<String>,
    /// 「自动」在已装模型里挑数值最小的：通用模型在前（速度与准确度均衡的优先），只认一种文字的语种模型在后。
    pub priority: u32,
    /// 根目录的默认模型：下载后放在根目录，不建子目录。
    #[serde(default)]
    pub default: bool,
    pub det: String,
    pub rec: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dict: Option<String>,
    pub files: Vec<RemoteFile>,
}

/// 动态加载 ONNX Runtime 的构建（C++ 轻量版）要下载的运行库，按架构（x64、arm64）各一份。
/// 取自 PyPI 上微软发布的 onnxruntime wheel，带微软签名；镜像站给的是同一个文件。
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogRuntime {
    pub arch: String,
    pub version: String,
    pub file: RemoteFile,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Catalog {
    pub schema: u32,
    pub models: Vec<CatalogModel>,
    pub runtimes: Vec<CatalogRuntime>,
}

/// 子目录里的 `model.json`。文件名都相对于该子目录。
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Manifest {
    pub name: String,
    pub det: String,
    pub rec: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dict: Option<String>,
    /// 从内置清单下载的模型记下清单 id，手动添加的为空。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub catalog_id: Option<String>,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct InstalledModel {
    /// 子目录名；根目录的默认模型为空串。
    pub id: String,
    pub name: String,
    pub det: PathBuf,
    pub rec: PathBuf,
    pub dict: Option<PathBuf>,
    pub catalog_id: Option<String>,
}

/// 编进库里的清单。清单随程序版本更新，不从网络拉取，地址和哈希都固定在这里。
pub fn catalog() -> &'static Catalog {
    static CATALOG: OnceLock<Catalog> = OnceLock::new();
    CATALOG.get_or_init(|| serde_json::from_str(include_str!("../catalog.json")).expect("内置模型清单无效"))
}

pub fn catalog_model(id: &str) -> Option<&'static CatalogModel> {
    catalog().models.iter().find(|m| m.id == id)
}

/// 列出 `root` 下文件齐全的模型：根目录的默认模型在前，子目录按名字排序。
/// model.json 无效或缺文件的子目录不列出。
pub fn discover(root: &Path) -> Vec<InstalledModel> {
    let mut found = Vec::new();
    let (det, rec) = (root.join(DEFAULT_DET), root.join(DEFAULT_REC));
    if det.is_file() && rec.is_file() {
        found.push(InstalledModel {
            id: String::new(),
            name: catalog().models.iter().find(|m| m.default).map_or("PP-OCRv6 small", |m| m.name.as_str()).to_string(),
            det,
            rec,
            dict: None,
            catalog_id: catalog().models.iter().find(|m| m.default).map(|m| m.id.clone()),
        });
    }
    let Ok(entries) = fs::read_dir(root) else {
        return found;
    };
    let mut dirs: Vec<String> = entries
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|name| valid_id(name))
        .collect();
    dirs.sort_by_key(|name| name.to_lowercase());
    found.extend(dirs.into_iter().filter_map(|id| load(root, &id)));
    found
}

/// 「自动」用的模型：已装的清单模型按 priority 取最小的，手动添加的模型排在所有清单模型之后。
pub fn auto_choice(root: &Path) -> Option<InstalledModel> {
    let rank = |m: &InstalledModel| {
        m.catalog_id.as_deref().and_then(catalog_model).map_or(u32::MAX, |c| c.priority)
    };
    discover(root).into_iter().min_by_key(rank)
}

/// 按 id 找已安装的模型；空串是根目录的默认模型。不存在时返回 None，调用方不应换用别的模型。
pub fn resolve(root: &Path, id: &str) -> Option<InstalledModel> {
    if id.is_empty() {
        return discover(root).into_iter().find(|m| m.id.is_empty());
    }
    valid_id(id).then(|| load(root, id)).flatten()
}

fn load(root: &Path, id: &str) -> Option<InstalledModel> {
    let dir = root.join(id);
    let manifest: Manifest = serde_json::from_slice(&fs::read(dir.join(MANIFEST)).ok()?).ok()?;
    let file = |name: &str| (valid_file_name(name) && dir.join(name).is_file()).then(|| dir.join(name));
    let dict = match &manifest.dict {
        Some(name) => Some(file(name)?),
        None => None,
    };
    Some(InstalledModel {
        id: id.to_string(),
        name: manifest.name,
        det: file(&manifest.det)?,
        rec: file(&manifest.rec)?,
        dict,
        catalog_id: manifest.catalog_id,
    })
}

/// 打开模型并各跑一次空白推理，确认这套文件在当前 ONNX Runtime 下能用。
pub fn validate(det: &Path, rec: &Path, dict: Option<&Path>) -> Result<(), String> {
    let path = |p: &Path| p.to_str().map(str::to_string).ok_or_else(|| format!("路径无效: {}", p.display()));
    let dict = dict.map(path).transpose()?;
    Engine::open_with_dict(&path(det)?, &path(rec)?, dict.as_deref())?.self_test()
}

/// 把用户选的模型文件复制成 `root` 下的一个新子目录。先复制到临时目录并校验，通过后才改名就位，
/// 失败时不留下半成品。需要 ONNX Runtime 已加载。
pub fn import(root: &Path, name: &str, det: &Path, rec: &Path, dict: Option<&Path>) -> Result<InstalledModel, String> {
    let name = name.trim();
    if name.is_empty() {
        return Err("模型名称不能为空".to_string());
    }
    let id = unique_id(root, name);
    let staging = root.join(format!(".import-{id}"));
    let result = (|| {
        if staging.exists() {
            fs::remove_dir_all(&staging).map_err(|e| format!("清理临时目录失败: {e}"))?;
        }
        fs::create_dir_all(&staging).map_err(|e| format!("创建模型目录失败: {e}"))?;
        let det_name = source_name(det)?;
        let mut rec_name = source_name(rec)?;
        if rec_name.eq_ignore_ascii_case(&det_name) {
            rec_name = format!("rec_{rec_name}");
        }
        let dict_name = match dict {
            Some(path) => {
                let n = source_name(path)?;
                Some(if n.eq_ignore_ascii_case(&det_name) || n.eq_ignore_ascii_case(&rec_name) { format!("dict_{n}") } else { n })
            }
            None => None,
        };
        let copy = |from: &Path, to: &str| fs::copy(from, staging.join(to)).map(|_| ()).map_err(|e| format!("复制 {} 失败: {e}", from.display()));
        copy(det, &det_name)?;
        copy(rec, &rec_name)?;
        if let (Some(from), Some(to)) = (dict, &dict_name) {
            copy(from, to)?;
        }
        validate(&staging.join(&det_name), &staging.join(&rec_name), dict_name.as_ref().map(|n| staging.join(n)).as_deref())?;
        let manifest = Manifest { name: name.to_string(), det: det_name, rec: rec_name, dict: dict_name, catalog_id: None };
        write_manifest(&staging, &manifest)?;
        fs::rename(&staging, root.join(&id)).map_err(|e| format!("模型目录就位失败: {e}"))?;
        load(root, &id).ok_or_else(|| "模型目录就位后读取失败".to_string())
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(&staging);
    }
    result
}

/// 更新器把清单模型 `catalog_id` 的文件下载并校验到 `root/.download-<catalog_id>/` 之后调用：
/// 默认模型移到根目录，其他模型写好 model.json 后整个目录改名就位，已有的同名模型被替换。
pub fn finish_download(root: &Path, catalog_id: &str) -> Result<InstalledModel, String> {
    let entry = catalog_model(catalog_id).ok_or_else(|| format!("清单里没有模型 {catalog_id}"))?;
    let staging = root.join(format!(".download-{catalog_id}"));
    for file in &entry.files {
        let size = fs::metadata(staging.join(&file.name)).map(|m| m.len()).unwrap_or(u64::MAX);
        if size != file.size {
            return Err(format!("下载的文件不完整: {}", file.name));
        }
    }
    if entry.default {
        for name in [&entry.det, &entry.rec] {
            fs::rename(staging.join(name), root.join(name)).map_err(|e| format!("模型文件就位失败: {e}"))?;
        }
        let _ = fs::remove_dir_all(&staging);
        return resolve(root, "").ok_or_else(|| "默认模型就位后读取失败".to_string());
    }
    let manifest = Manifest {
        name: entry.name.clone(),
        det: entry.det.clone(),
        rec: entry.rec.clone(),
        dict: entry.dict.clone(),
        catalog_id: Some(entry.id.clone()),
    };
    write_manifest(&staging, &manifest)?;
    let target = root.join(&entry.id);
    if target.exists() {
        fs::remove_dir_all(&target).map_err(|e| format!("替换旧模型失败: {e}"))?;
    }
    fs::rename(&staging, &target).map_err(|e| format!("模型目录就位失败: {e}"))?;
    load(root, &entry.id).ok_or_else(|| "模型目录就位后读取失败".to_string())
}

/// 运行库下载到 `root/.download-runtime/` 之后调用，把它移到 `root/onnxruntime.dll`。
/// 已加载的运行库不能替换，调用方只在还没加载时下载。
pub fn finish_runtime_download(root: &Path) -> Result<PathBuf, String> {
    let staging = root.join(RUNTIME_STAGING);
    let target = root.join(RUNTIME_DLL);
    fs::rename(staging.join(RUNTIME_DLL), &target).map_err(|e| format!("运行库就位失败: {e}"))?;
    let _ = fs::remove_dir_all(&staging);
    Ok(target)
}

/// 删除一个模型：子目录模型删整个目录，默认模型（空 id）删根目录的两个模型文件，运行库和别的模型不动。
pub fn remove(root: &Path, id: &str) -> Result<(), String> {
    if id.is_empty() {
        for name in [DEFAULT_DET, DEFAULT_REC] {
            let path = root.join(name);
            if path.is_file() {
                fs::remove_file(&path).map_err(|e| format!("删除模型失败: {e}"))?;
            }
        }
        return Ok(());
    }
    if !valid_id(id) {
        return Err(format!("模型 id 无效: {id:?}"));
    }
    let dir = root.join(id);
    if !dir.join(MANIFEST).is_file() {
        return Err(format!("不是模型目录: {}", dir.display()));
    }
    fs::remove_dir_all(&dir).map_err(|e| format!("删除模型失败: {e}"))
}

/// 删掉上次没做完的下载和添加留下的临时目录。只在没有下载进行时调用（启动时）。
pub fn clean_staging(root: &Path) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    for entry in entries.filter_map(|e| e.ok()) {
        let name = entry.file_name().to_string_lossy().into_owned();
        if entry.file_type().is_ok_and(|t| t.is_dir()) && STAGING_PREFIXES.iter().any(|p| name.starts_with(p)) {
            let _ = fs::remove_dir_all(entry.path());
        }
    }
}

fn write_manifest(dir: &Path, manifest: &Manifest) -> Result<(), String> {
    let text = serde_json::to_string_pretty(manifest).map_err(|e| e.to_string())?;
    fs::write(dir.join(MANIFEST), text + "\n").map_err(|e| format!("写入 {MANIFEST} 失败: {e}"))
}

fn source_name(path: &Path) -> Result<String, String> {
    path.file_name()
        .and_then(|n| n.to_str())
        .filter(|n| valid_file_name(n))
        .map(str::to_string)
        .ok_or_else(|| format!("文件名无效: {}", path.display()))
}

const RESERVED: [&str; 22] = [
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2",
    "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
];

/// Windows 下能直接作为一层文件名的字符串：不含路径分隔符和保留字符，不以点或空格结尾，不是设备名。
fn valid_file_name(name: &str) -> bool {
    !name.is_empty()
        && name.chars().count() <= 64
        && !name.chars().any(|c| c.is_control() || "<>:\"/\\|?*".contains(c))
        && !name.ends_with(['.', ' '])
        && name != ".."
        && !RESERVED.contains(&name.split('.').next().unwrap_or("").to_ascii_uppercase().as_str())
}

/// 子目录模型的 id：合法文件名，且不以点开头（点开头的是临时目录）。
fn valid_id(id: &str) -> bool {
    valid_file_name(id) && !id.starts_with('.')
}

/// 由显示名生成一个还没被占用的 id：非法字符换成 `_`，重名时加 `-2`、`-3`……
fn unique_id(root: &Path, name: &str) -> String {
    let mut base: String = name
        .chars()
        .map(|c| if c.is_control() || "<>:\"/\\|?*".contains(c) { '_' } else { c })
        .take(56)
        .collect();
    base = base.trim_matches(|c: char| c == '.' || c == ' ').to_string();
    if base.is_empty() {
        base = "model".to_string();
    } else if !valid_id(&base) {
        base = format!("model_{base}");
        if !valid_id(&base) {
            base = "model".to_string();
        }
    }
    let mut id = base.clone();
    let mut n = 2;
    while root.join(&id).exists() {
        id = format!("{base}-{n}");
        n += 1;
    }
    id
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TempDir(PathBuf);
    impl TempDir {
        fn new(tag: &str) -> Self {
            let dir = std::env::temp_dir().join(format!("ppocr-models-{tag}-{}", std::process::id()));
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            TempDir(dir)
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn write(path: &Path, bytes: &[u8]) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, bytes).unwrap();
    }

    #[test]
    fn catalog_is_consistent() {
        let c = catalog();
        assert_eq!(c.schema, 1);
        let defaults: Vec<_> = c.models.iter().filter(|m| m.default).collect();
        assert_eq!(defaults.len(), 1);
        assert_eq!((defaults[0].det.as_str(), defaults[0].rec.as_str()), (DEFAULT_DET, DEFAULT_REC));
        let mut ids: Vec<_> = c.models.iter().map(|m| m.id.as_str()).collect();
        ids.sort();
        ids.dedup();
        assert_eq!(ids.len(), c.models.len());
        let mut priorities: Vec<_> = c.models.iter().map(|m| m.priority).collect();
        priorities.sort();
        priorities.dedup();
        assert_eq!(priorities.len(), c.models.len());
        assert_eq!(c.models.iter().min_by_key(|m| m.priority).unwrap().id, defaults[0].id);
        for m in &c.models {
            assert!(valid_id(&m.id), "{}", m.id);
            let names: Vec<_> = m.files.iter().map(|f| f.name.as_str()).collect();
            assert!(names.contains(&m.det.as_str()) && names.contains(&m.rec.as_str()), "{}", m.id);
            if let Some(dict) = &m.dict {
                assert!(names.contains(&dict.as_str()), "{}", m.id);
            }
            for f in &m.files {
                assert!(valid_file_name(&f.name) && f.size > 0 && !f.urls.is_empty());
                assert!(f.sha256.len() == 64 && f.sha256.chars().all(|c| c.is_ascii_digit() || ('a'..='f').contains(&c)));
                assert!(f.urls.iter().all(|u| u.starts_with("https://")));
            }
        }
        let arches: Vec<_> = c.runtimes.iter().map(|r| r.arch.as_str()).collect();
        assert_eq!(arches, ["x64", "arm64"]);
        for r in &c.runtimes {
            assert_eq!(r.file.name, RUNTIME_DLL);
            assert!(r.version.starts_with("1.") && r.version[2..].split('.').next().unwrap().parse::<u32>().unwrap() >= 24);
            let archive = r.file.archive.as_ref().expect("运行库取自 wheel");
            assert!(archive.member.ends_with(&format!("/{RUNTIME_DLL}")) && archive.size > 0);
            assert!(archive.sha256.len() == 64 && r.file.urls.iter().all(|u| u.starts_with("https://") && u.ends_with(".whl")));
        }
    }

    #[test]
    fn discover_lists_default_then_valid_subdirectories() {
        let t = TempDir::new("discover");
        let root = &t.0;
        assert!(discover(root).is_empty());
        write(&root.join(DEFAULT_DET), b"d");
        write(&root.join(DEFAULT_REC), b"r");
        let ok = Manifest { name: "Korean".into(), det: "det.onnx".into(), rec: "rec.onnx".into(), dict: None, catalog_id: Some("ppocr-v5-korean".into()) };
        for (dir, manifest) in [("b-korean", &ok), ("A-local", &Manifest { catalog_id: None, name: "Local".into(), ..ok.clone() })] {
            write(&root.join(dir).join("det.onnx"), b"d");
            write(&root.join(dir).join("rec.onnx"), b"r");
            write_manifest(&root.join(dir), manifest).unwrap();
        }
        // 缺识别模型、model.json 损坏、临时目录、字典指到目录外：都不列出
        write(&root.join("missing").join("det.onnx"), b"d");
        write_manifest(&root.join("missing"), &ok).unwrap();
        write(&root.join("broken").join(MANIFEST), b"{");
        write(&root.join(".download-x").join("det.onnx"), b"d");
        write_manifest(&root.join(".download-x"), &ok).unwrap();
        write(&root.join("escape").join("det.onnx"), b"d");
        write(&root.join("escape").join("rec.onnx"), b"r");
        write_manifest(&root.join("escape"), &Manifest { dict: Some("..\\x.txt".into()), ..ok.clone() }).unwrap();

        let found = discover(root);
        let ids: Vec<_> = found.iter().map(|m| m.id.as_str()).collect();
        assert_eq!(ids, ["", "A-local", "b-korean"]);
        assert_eq!(found[0].catalog_id.as_deref(), Some("ppocr-v6-small"));
        assert_eq!(resolve(root, "b-korean").unwrap().rec, root.join("b-korean").join("rec.onnx"));
        assert!(resolve(root, "missing").is_none() && resolve(root, "..").is_none() && resolve(root, ".download-x").is_none());
    }

    #[test]
    fn auto_choice_prefers_general_catalog_models_then_local_ones() {
        let t = TempDir::new("auto");
        let root = &t.0;
        assert!(auto_choice(root).is_none());
        let add = |dir: &str, catalog_id: Option<&str>| {
            write(&root.join(dir).join("det.onnx"), b"d");
            write(&root.join(dir).join("rec.onnx"), b"r");
            let m = Manifest { name: dir.into(), det: "det.onnx".into(), rec: "rec.onnx".into(), dict: None, catalog_id: catalog_id.map(Into::into) };
            write_manifest(&root.join(dir), &m).unwrap();
        };
        add("A-local", None);
        assert_eq!(auto_choice(root).unwrap().id, "A-local");
        add("ppocr-v5-korean", Some("ppocr-v5-korean"));
        assert_eq!(auto_choice(root).unwrap().id, "ppocr-v5-korean");
        add("ppocr-v6-tiny", Some("ppocr-v6-tiny"));
        assert_eq!(auto_choice(root).unwrap().id, "ppocr-v6-tiny");
        add("ppocr-v6-medium", Some("ppocr-v6-medium"));
        assert_eq!(auto_choice(root).unwrap().id, "ppocr-v6-medium");
        write(&root.join(DEFAULT_DET), b"d");
        write(&root.join(DEFAULT_REC), b"r");
        assert_eq!(auto_choice(root).unwrap().id, "");
    }

    #[test]
    fn finish_download_places_subdirectory_and_default_models() {
        let t = TempDir::new("finish");
        let root = &t.0;
        let korean = catalog_model("ppocr-v5-korean").unwrap();
        let staging = root.join(".download-ppocr-v5-korean");
        for f in &korean.files {
            write(&staging.join(&f.name), &vec![0u8; f.size as usize - 1]);
        }
        assert!(finish_download(root, "ppocr-v5-korean").unwrap_err().contains("不完整"));
        for f in &korean.files {
            write(&staging.join(&f.name), &vec![0u8; f.size as usize]);
        }
        write(&root.join("ppocr-v5-korean").join("old.onnx"), b"old");
        let installed = finish_download(root, "ppocr-v5-korean").unwrap();
        assert_eq!(installed.catalog_id.as_deref(), Some("ppocr-v5-korean"));
        assert!(!staging.exists() && !root.join("ppocr-v5-korean").join("old.onnx").exists());

        let small = catalog_model("ppocr-v6-small").unwrap();
        for f in &small.files {
            write(&root.join(".download-ppocr-v6-small").join(&f.name), &vec![0u8; f.size as usize]);
        }
        assert_eq!(finish_download(root, "ppocr-v6-small").unwrap().id, "");
        assert!(root.join(DEFAULT_DET).is_file() && !root.join(".download-ppocr-v6-small").exists());
        assert!(finish_download(root, "nope").is_err());
    }

    #[test]
    fn remove_deletes_only_the_model_files() {
        let t = TempDir::new("remove");
        let root = &t.0;
        write(&root.join(DEFAULT_DET), b"d");
        write(&root.join(DEFAULT_REC), b"r");
        write(&root.join(RUNTIME_DLL), b"x");
        write(&root.join("plain").join("a.txt"), b"a");
        write(&root.join("m").join(MANIFEST), b"{}");
        assert!(remove(root, "..").is_err() && remove(root, "a/b").is_err());
        assert!(remove(root, "plain").is_err() && root.join("plain").exists());
        remove(root, "m").unwrap();
        assert!(!root.join("m").exists() && root.join(DEFAULT_DET).exists());
        remove(root, "").unwrap();
        assert!(!root.join(DEFAULT_DET).exists() && !root.join(DEFAULT_REC).exists());
        assert!(root.join(RUNTIME_DLL).exists() && root.join("plain").exists());
    }

    #[test]
    fn clean_staging_keeps_models() {
        let t = TempDir::new("clean");
        let root = &t.0;
        for dir in [".download-a", ".import-b", "keep", ".other"] {
            write(&root.join(dir).join("f"), b"x");
        }
        clean_staging(root);
        assert!(!root.join(".download-a").exists() && !root.join(".import-b").exists());
        assert!(root.join("keep").exists() && root.join(".other").exists());
    }

    /// 用仓库根目录 `models/` 里的默认模型跑真实加载；静态链接 ONNX Runtime 时才跑得了。
    #[cfg(not(feature = "load-dynamic"))]
    #[test]
    fn import_validates_before_placing_the_model() {
        let repo_models = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../models");
        let (det, rec) = (repo_models.join(DEFAULT_DET), repo_models.join(DEFAULT_REC));
        let t = TempDir::new("import");
        let root = &t.0;
        let model = import(root, "我的模型", &det, &rec, None).unwrap();
        assert_eq!((model.id.as_str(), model.dict.as_ref()), ("我的模型", None));
        assert_eq!(fs::metadata(&model.rec).unwrap().len(), fs::metadata(&rec).unwrap().len());
        assert_eq!(discover(root).iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["我的模型"]);

        let dict = root.join("short.txt");
        write(&dict, "a\nb\nc\n".as_bytes());
        assert!(import(root, "bad dict", &det, &rec, Some(&dict)).unwrap_err().contains("对不上"));
        assert!(import(root, "swapped", &rec, &det, None).is_err());
        let left: Vec<_> = fs::read_dir(root).unwrap().filter_map(|e| e.ok()).map(|e| e.file_name()).collect();
        assert_eq!(left.len(), 2, "{left:?}"); // 只剩添加成功的模型和字典文件
        assert_eq!(import(root, "我的模型", &det, &rec, None).unwrap().id, "我的模型-2");
    }

    #[test]
    fn unique_id_sanitizes_and_avoids_collisions() {
        let t = TempDir::new("unique");
        let root = &t.0;
        assert_eq!(unique_id(root, "我的 模型"), "我的 模型");
        assert_eq!(unique_id(root, "a/b:c"), "a_b_c");
        assert_eq!(unique_id(root, "..."), "model");
        assert_eq!(unique_id(root, "CON"), "model_CON");
        assert_eq!(unique_id(root, ".hidden"), "hidden");
        fs::create_dir(root.join("x")).unwrap();
        fs::create_dir(root.join("x-2")).unwrap();
        assert_eq!(unique_id(root, "x"), "x-3");
    }
}
