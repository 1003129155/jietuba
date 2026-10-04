use crate::{contract::Arch, fail, Result};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::{Read, Seek, SeekFrom},
    path::Path,
    process::Command,
    time::{Duration, Instant},
};

pub fn is_reparse(meta: &fs::Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        meta.file_attributes() & 0x400 != 0
    }
    #[cfg(not(windows))]
    {
        let _ = meta;
        false
    }
}

pub fn durable_rename(source: &Path, target: &Path) -> Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Storage::FileSystem::{
            MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
        };
        let from: Vec<u16> = source.as_os_str().encode_wide().chain(Some(0)).collect();
        let to: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
        for attempt in 0..5 {
            if unsafe {
                MoveFileExW(
                    from.as_ptr(),
                    to.as_ptr(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
                )
            } != 0
            {
                return Ok(());
            }
            let error = std::io::Error::last_os_error();
            // Retry transient antivirus or indexing locks without bypassing permissions or terminating processes.
            if attempt == 4 || !matches!(error.raw_os_error(), Some(5 | 32 | 33)) {
                let mut error: crate::Error = error.into();
                error.message = format!("原子替换 {}: {}", target.display(), error.message);
                return Err(error);
            }
            std::thread::sleep(Duration::from_millis(50 * (1 << attempt)));
        }
        unreachable!()
    }
    #[cfg(not(windows))]
    {
        fs::rename(source, target)?;
        fs::File::open(target.parent().unwrap())?.sync_all()?;
        Ok(())
    }
}
pub fn pe_arch(path: &Path) -> Result<Arch> {
    let mut file = fs::File::open(path)?;
    let mut header = [0u8; 64];
    file.read_exact(&mut header)?;
    if &header[..2] != b"MZ" {
        return fail("architecture", "文件不是 Windows PE");
    }
    let offset = u32::from_le_bytes(header[60..64].try_into().unwrap()) as u64;
    if offset < 64 || offset > file.metadata()?.len().saturating_sub(26) {
        return fail("architecture", "PE 偏移无效");
    }
    file.seek(SeekFrom::Start(offset))?;
    let mut pe = [0u8; 26];
    file.read_exact(&mut pe)?;
    if &pe[..4] != b"PE\0\0" {
        return fail("architecture", "PE 签名无效");
    }
    let sections = u16::from_le_bytes([pe[6], pe[7]]) as u64;
    let optional = u16::from_le_bytes([pe[20], pe[21]]) as u64;
    let flags = u16::from_le_bytes([pe[22], pe[23]]);
    if sections == 0
        || sections > 96
        || optional < 112
        || flags & 2 == 0
        || flags & 0x2000 != 0
        || pe[24..26] != [0x0b, 0x02]
        || offset + 24 + optional + sections * 40 > file.metadata()?.len()
    {
        return fail("architecture", "文件不是有效的 64 位 Windows EXE");
    }
    match u16::from_le_bytes([pe[4], pe[5]]) {
        0x8664 => Ok(Arch::X64),
        0xaa64 => Ok(Arch::Arm64),
        _ => fail("architecture", "仅支持 Windows x64/ARM64"),
    }
}
pub fn host_arch() -> Arch {
    if cfg!(target_arch = "aarch64") {
        Arch::Arm64
    } else {
        Arch::X64
    }
}
pub fn independent_command(exe: &Path) -> Command {
    let mut command = Command::new(exe);
    command.env("PYINSTALLER_RESET_ENVIRONMENT", "1");
    // Do not inherit the old onefile unpack directory or loader state.
    for (key, value) in std::env::vars_os() {
        if key.to_string_lossy().starts_with("_PYI_") || key == "_MEIPASS2" {
            command.env_remove(key);
        } else if key.to_string_lossy().eq_ignore_ascii_case("PATH") {
            let clean = std::env::split_paths(&value)
                .filter(|p| {
                    !p.components()
                        .any(|part| part.as_os_str().to_string_lossy().starts_with("_MEI"))
                })
                .collect::<Vec<_>>();
            if let Ok(path) = std::env::join_paths(clean) {
                command.env(key, path);
            }
        }
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);
    }
    command
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProcessIdentity {
    pub pid: u32,
    pub created: u64,
    pub executable: String,
}

#[cfg(windows)]
mod win {
    use super::*;
    use windows_sys::Win32::{
        Foundation::{CloseHandle, FILETIME, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT},
        System::Threading::{
            GetProcessTimes, OpenProcess, QueryFullProcessImageNameW, WaitForSingleObject,
            PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE,
        },
    };
    pub struct Process(HANDLE);
    impl Drop for Process {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }
    pub fn open(identity: &ProcessIdentity) -> Result<Process> {
        let handle = unsafe {
            OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE,
                0,
                identity.pid,
            )
        };
        if handle.is_null() {
            return fail("process", "无法打开原程序进程，不能确认退出");
        }
        let process = Process(handle);
        let mut created: FILETIME = unsafe { std::mem::zeroed() };
        let mut exited = created;
        let mut kernel = created;
        let mut user = created;
        let mut path = vec![0u16; 32768];
        let mut size = path.len() as u32;
        unsafe {
            if GetProcessTimes(handle, &mut created, &mut exited, &mut kernel, &mut user) == 0
                || QueryFullProcessImageNameW(handle, 0, path.as_mut_ptr(), &mut size) == 0
            {
                return fail("process", "无法校验进程身份");
            }
        }
        let ticks = ((created.dwHighDateTime as u64) << 32) | created.dwLowDateTime as u64;
        if ticks != identity.created
            || !same_path(
                &String::from_utf16_lossy(&path[..size as usize]),
                &identity.executable,
            )
        {
            return fail("process", "原程序 PID 已复用或路径不匹配");
        }
        Ok(process)
    }
    impl Process {
        pub fn exited(&self) -> Result<bool> {
            match unsafe { WaitForSingleObject(self.0, 0) } {
                WAIT_OBJECT_0 => Ok(true),
                WAIT_TIMEOUT => Ok(false),
                _ => fail("process", "等待进程失败"),
            }
        }
    }
}
pub fn same_path(left: &str, right: &str) -> bool {
    left.trim_start_matches("\\\\?\\")
        .eq_ignore_ascii_case(right.trim_start_matches("\\\\?\\"))
}

pub fn sanitize_loader() -> Result<()> {
    #[cfg(windows)]
    if unsafe { windows_sys::Win32::System::LibraryLoader::SetDllDirectoryW(std::ptr::null()) } == 0
    {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(())
}

pub fn app_processes(executable: &Path) -> Result<Vec<ProcessIdentity>> {
    #[cfg(not(windows))]
    {
        let _ = executable;
        fail("platform", "进程交接仅支持 Windows")
    }
    #[cfg(windows)]
    {
        use windows_sys::Win32::{
            Foundation::{CloseHandle, FILETIME, INVALID_HANDLE_VALUE},
            System::{
                Diagnostics::ToolHelp::{
                    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
                    TH32CS_SNAPPROCESS,
                },
                Threading::{
                    GetProcessTimes, OpenProcess, QueryFullProcessImageNameW,
                    PROCESS_QUERY_LIMITED_INFORMATION,
                },
            },
        };
        let expected = executable.to_string_lossy();
        let filename = executable.file_name().unwrap().to_string_lossy();
        let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
        if snapshot == INVALID_HANDLE_VALUE {
            return Err(std::io::Error::last_os_error().into());
        }
        let result = (|| {
            let mut entry: PROCESSENTRY32W = unsafe { std::mem::zeroed() };
            entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
            let mut found = Vec::new();
            let mut ok = unsafe { Process32FirstW(snapshot, &mut entry) };
            while ok != 0 {
                let name = String::from_utf16_lossy(
                    &entry.szExeFile[..entry
                        .szExeFile
                        .iter()
                        .position(|x| *x == 0)
                        .unwrap_or(entry.szExeFile.len())],
                );
                if name.eq_ignore_ascii_case(&filename) {
                    let handle = unsafe {
                        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, entry.th32ProcessID)
                    };
                    if handle.is_null() {
                        return fail("process", "同名程序进程无法校验，取消升级");
                    }
                    let mut path = vec![0u16; 32768];
                    let mut n = path.len() as u32;
                    let mut created: FILETIME = unsafe { std::mem::zeroed() };
                    let mut exit = created;
                    let mut kernel = created;
                    let mut user = created;
                    let valid = unsafe {
                        QueryFullProcessImageNameW(handle, 0, path.as_mut_ptr(), &mut n) != 0
                            && GetProcessTimes(
                                handle,
                                &mut created,
                                &mut exit,
                                &mut kernel,
                                &mut user,
                            ) != 0
                    };
                    unsafe {
                        CloseHandle(handle);
                    }
                    if !valid {
                        return fail("process", "无法校验同名程序路径");
                    }
                    let path = String::from_utf16_lossy(&path[..n as usize]);
                    if same_path(&expected, &path) {
                        found.push(ProcessIdentity {
                            pid: entry.th32ProcessID,
                            created: ((created.dwHighDateTime as u64) << 32)
                                | created.dwLowDateTime as u64,
                            executable: path,
                        });
                    }
                }
                ok = unsafe { Process32NextW(snapshot, &mut entry) };
            }
            Ok(found)
        })();
        unsafe {
            CloseHandle(snapshot);
        }
        result
    }
}
#[cfg(windows)]
pub use win::Process;
#[cfg(not(windows))]
pub struct Process;
pub fn open_process(identity: &ProcessIdentity) -> Result<Process> {
    #[cfg(windows)]
    {
        win::open(identity)
    }
    #[cfg(not(windows))]
    {
        let _ = identity;
        fail("platform", "进程交接仅支持 Windows")
    }
}
#[cfg(not(windows))]
impl Process {
    pub fn exited(&self) -> Result<bool> {
        fail("platform", "仅支持 Windows")
    }
}
pub fn wait_original(processes: &[Process], timeout: Duration) -> Result<()> {
    let start = Instant::now();
    loop {
        let mut done = true;
        for p in processes {
            done &= p.exited()?;
        }
        if done {
            return Ok(());
        }
        if start.elapsed() >= timeout {
            return fail("exit_timeout", "原程序尚未退出，取消升级");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}
