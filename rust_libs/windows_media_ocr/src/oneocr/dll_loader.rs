//! oneocr.dll 文件发现和加载
//!
//! 自动从 Windows ScreenSketch 应用目录查找并复制 DLL 文件，
//! 替代 Python 版本中用 PowerShell 查找的方案（更快、更可靠）。

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// oneocr 需要的三个文件
const REQUIRED_FILES: &[&str] = &["oneocr.dll", "oneocr.onemodel", "onnxruntime.dll"];

/// 获取 DLL 缓存目录（%TEMP%/Jietuba/oneocr_dlls）
fn get_cache_dir() -> PathBuf {
    let temp = env::temp_dir();
    temp.join("Jietuba").join("oneocr_dlls")
}

/// 检查目录中是否包含所有必需文件
fn has_all_files(dir: &Path) -> bool {
    REQUIRED_FILES
        .iter()
        .all(|f| dir.join(f).exists())
}

/// 使用 PowerShell 查找 ScreenSketch 安装路径
///
/// 这是备选方案，超时 5 秒。
fn find_screen_sketch_powershell() -> Option<PathBuf> {
    let output = Command::new("powershell")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "Get-AppxPackage Microsoft.ScreenSketch | Select-Object -ExpandProperty InstallLocation",
        ])
        .creation_flags(0x08000000) // CREATE_NO_WINDOW
        .output()
        .ok()?;

    if !output.status.success() {
        return None;
    }

    let install_location = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if install_location.is_empty() {
        return None;
    }

    let snipping_tool = PathBuf::from(&install_location).join("SnippingTool");
    if snipping_tool.exists() {
        Some(snipping_tool)
    } else {
        None
    }
}

/// 尝试在常见路径中查找 ScreenSketch
///
/// 比 PowerShell 快得多（直接检查文件系统）
fn find_screen_sketch_fast() -> Option<PathBuf> {
    // ProgramFiles\WindowsApps 下搜索
    let program_files = env::var("ProgramFiles").ok()?;
    let windows_apps = PathBuf::from(program_files).join("WindowsApps");

    if !windows_apps.exists() {
        return None;
    }

    // 枚举 WindowsApps 下匹配的目录
    let entries = fs::read_dir(&windows_apps).ok()?;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if name.starts_with("Microsoft.ScreenSketch_") {
            let snipping_tool = entry.path().join("SnippingTool");
            if snipping_tool.exists() && has_all_files(&snipping_tool) {
                return Some(snipping_tool);
            }
        }
    }

    None
}

/// 将文件从源目录复制到缓存目录
fn copy_files_to_cache(source: &Path, cache: &Path) -> Result<(), String> {
    fs::create_dir_all(cache).map_err(|e| format!("创建缓存目录失败: {}", e))?;

    for filename in REQUIRED_FILES {
        let src = source.join(filename);
        let dst = cache.join(filename);
        if !dst.exists() && src.exists() {
            fs::copy(&src, &dst)
                .map_err(|e| format!("复制 {} 失败: {}", filename, e))?;
        }
    }

    if has_all_files(cache) {
        Ok(())
    } else {
        Err("复制后文件不完整".to_string())
    }
}

/// 查找并准备 oneocr DLL 文件目录
///
/// 返回包含 oneocr.dll、oneocr.onemodel、onnxruntime.dll 的目录路径。
///
/// 查找顺序：
/// 1. 缓存目录（已经复制过的）
/// 2. 快速文件系统搜索 WindowsApps
/// 3. PowerShell 查询（备选）
pub fn find_and_prepare_dll_dir() -> Result<PathBuf, String> {
    let cache_dir = get_cache_dir();

    // 1. 缓存已存在
    if has_all_files(&cache_dir) {
        return Ok(cache_dir);
    }

    // 2. 快速搜索
    if let Some(source) = find_screen_sketch_fast() {
        if copy_files_to_cache(&source, &cache_dir).is_ok() {
            return Ok(cache_dir);
        }
    }

    // 3. PowerShell 备选
    if let Some(source) = find_screen_sketch_powershell() {
        if copy_files_to_cache(&source, &cache_dir).is_ok() {
            return Ok(cache_dir);
        }
    }

    Err(
        "无法找到 oneocr.dll。请确保已安装 Windows 截图工具 (Snipping Tool)".to_string(),
    )
}

/// DLL 目录路径（用于 SetDllDirectoryW）
pub fn get_dll_dir_path() -> Result<PathBuf, String> {
    find_and_prepare_dll_dir()
}

// 为 Command 添加 creation_flags 支持
#[cfg(windows)]
trait CommandExt {
    fn creation_flags(&mut self, flags: u32) -> &mut Self;
}

#[cfg(windows)]
impl CommandExt for Command {
    fn creation_flags(&mut self, flags: u32) -> &mut Self {
        use std::os::windows::process::CommandExt as WinExt;
        WinExt::creation_flags(self, flags);
        self
    }
}
