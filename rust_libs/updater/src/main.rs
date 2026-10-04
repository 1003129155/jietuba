use jietuba_updater::{
    self as core, archive, cache as cache_files, contract, download, platform, protocol,
    transaction, Result,
};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    collections::HashMap,
    fs, io,
    path::{Path, PathBuf},
    process::Stdio,
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
    time::{Duration, Instant},
};

#[derive(Serialize, Deserialize)]
struct Prepared {
    executable: PathBuf,
    current_version: String,
    release: contract::Release,
    staged_hash: String,
}
#[derive(Serialize, Deserialize)]
struct WorkerRequest {
    executable: PathBuf,
    staged: PathBuf,
    staged_hash: String,
    processes: Vec<platform::ProcessIdentity>,
    launcher: platform::ProcessIdentity,
    failure_message: Option<String>,
}

fn options() -> Result<(String, HashMap<String, String>)> {
    let mut args = std::env::args().skip(1);
    let command = args.next().unwrap_or_else(|| "--version".into());
    let mut options = HashMap::new();
    while let Some(key) = args.next() {
        if !key.starts_with("--") || options.contains_key(&key) {
            return core::fail("arguments", "参数无效或重复");
        }
        options.insert(
            key,
            args.next().ok_or_else(|| core::Error {
                code: "arguments",
                message: "参数缺少值".into(),
            })?,
        );
    }
    Ok((command, options))
}
fn arg<'a>(args: &'a HashMap<String, String>, key: &str) -> Result<&'a str> {
    args.get(key)
        .map(String::as_str)
        .ok_or_else(|| core::Error {
            code: "arguments",
            message: format!("缺少 {key}"),
        })
}
fn variant(args: &HashMap<String, String>) -> Result<contract::AppVariant> {
    contract::AppVariant::parse(arg(args, "--variant")?)
}
fn cache(args: &HashMap<String, String>) -> Result<PathBuf> {
    let root = if let Some(path) = args.get("--cache-dir") {
        PathBuf::from(path)
    } else {
        PathBuf::from(std::env::var_os("LOCALAPPDATA").ok_or_else(|| core::Error {
            code: "path",
            message: "LOCALAPPDATA 不可用".into(),
        })?)
        .join("jietuba/updater")
    };
    if !root.is_absolute() {
        return core::fail("path", "缓存路径必须为绝对路径");
    }
    fs::create_dir_all(&root)?;
    Ok(fs::canonicalize(root)?)
}
fn control(id: &str) -> mpsc::Receiver<Result<String>> {
    let (send, receive) = mpsc::channel();
    let id = id.to_owned();
    std::thread::spawn(move || {
        let value = protocol::read_control(&mut io::stdin().lock(), &id).map(|c| c.command);
        let _ = send.send(value);
    });
    receive
}
fn cancellable(id: &str) -> Arc<AtomicBool> {
    let receive = control(id);
    let flag = Arc::new(AtomicBool::new(false));
    let copy = flag.clone();
    std::thread::spawn(move || {
        let _ = receive.recv();
        copy.store(true, Ordering::Release);
    });
    flag
}
fn runtime() -> Result<tokio::runtime::Runtime> {
    Ok(tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()?)
}
fn wait_file(dir: &Path, name: &str, timeout: Duration) -> Result<()> {
    let started = Instant::now();
    loop {
        if core::safe_join(dir, name)?.exists() {
            return Ok(());
        }
        let result = core::safe_join(dir, "result.json")?;
        if result.exists() {
            let data: serde_json::Value = serde_json::from_slice(&fs::read(result)?)?;
            return core::fail(
                "worker",
                data["message"].as_str().unwrap_or("更新 worker 已结束"),
            );
        }
        if started.elapsed() > timeout {
            return core::fail("timeout", "更新交接超时");
        }
        std::thread::sleep(Duration::from_millis(25));
    }
}
fn apply(args: &HashMap<String, String>, executable: &Path, id: &str) -> Result<()> {
    core::valid_id(id)?;
    let root = cache(args)?;
    let stage_dir = core::safe_join(&root, id)?;
    let prepared: Prepared =
        serde_json::from_slice(&fs::read(core::safe_join(&stage_dir, "prepared.json")?)?)?;
    if prepared.executable != executable
        || prepared.current_version != arg(args, "--current-version")?
        || prepared.release.asset_name
            != variant(args)?.asset_name(&prepared.release.tag_name, platform::pe_arch(executable)?)
        || contract::version(&prepared.release.tag_name)?
            <= contract::version(&prepared.current_version)?
    {
        return core::fail("version", "下载结果不属于当前安装或版本");
    }
    let worker_id = core::random_id();
    let dir = core::safe_join(&root, &worker_id)?;
    fs::create_dir(&dir)?;
    let processes = platform::app_processes(executable)?;
    if let Some(pid) = args.get("--parent-pid") {
        let pid: u32 = pid.parse().map_err(|_| core::Error {
            code: "arguments",
            message: "进程号无效".into(),
        })?;
        if !processes.iter().any(|p| p.pid == pid) {
            return core::fail("process", "调用进程不属于当前主程序");
        }
    }
    core::write_json(
        &core::safe_join(&dir, "request.json")?,
        &WorkerRequest {
            executable: executable.into(),
            staged: core::safe_join(&stage_dir, "app.exe")?,
            staged_hash: prepared.staged_hash,
            processes,
            launcher: platform::app_processes(&std::env::current_exe()?)?
                .into_iter()
                .find(|p| p.pid == std::process::id())
                .ok_or_else(|| core::Error {
                    code: "process",
                    message: "无法确认交接进程身份".into(),
                })?,
            failure_message: args.get("--failure-message").cloned(),
        },
    )?;
    let worker_exe = core::safe_join(&dir, "jietuba_updater.exe")?;
    fs::copy(std::env::current_exe()?, &worker_exe)?;
    let mut command = platform::independent_command(&worker_exe);
    command
        .arg("worker")
        .arg("--worker-dir")
        .arg(&dir)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    let mut child = command.spawn()?;
    let result = (|| {
        wait_file(&dir, "ready.json", Duration::from_secs(30))?;
        protocol::emit("ready", id, json!({"worker":worker_id}))?;
        let command = control(id)
            .recv_timeout(Duration::from_secs(60))
            .map_err(|_| core::Error {
                code: "cancelled",
                message: "交接已取消或超时".into(),
            })??;
        if command != "go" {
            return core::fail("cancelled", "更新已取消");
        }
        core::write_json(&core::safe_join(&dir, "go.json")?, &json!({"go":true}))?;
        wait_file(&dir, "accepted.json", Duration::from_secs(10))?;
        protocol::emit(
            "handed_off",
            id,
            json!({"worker":worker_id,"result":dir.join("result.json")}),
        )?;
        Ok(())
    })();
    if result.is_err() && !dir.join("accepted.json").exists() {
        let _ = core::write_json(
            &core::safe_join(&dir, "cancel.json")?,
            &json!({"cancel":true}),
        );
        for _ in 0..100 {
            if child.try_wait()?.is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
    }
    result
}
fn worker(dir: &Path) -> Result<()> {
    let request: WorkerRequest =
        serde_json::from_slice(&fs::read(core::safe_join(dir, "request.json")?)?)?;
    let executable = transaction::install_exe(&request.executable)?;
    let _lock = transaction::lock(&executable)?;
    if core::hash_file(&request.staged)? != request.staged_hash {
        return core::fail("conflict", "下载结果已改变");
    }
    let handles = request
        .processes
        .iter()
        .map(platform::open_process)
        .collect::<Result<Vec<_>>>()?;
    let launcher = platform::open_process(&request.launcher)?;
    let id = dir.file_name().unwrap().to_string_lossy();
    let mut journal = transaction::prepare(&executable, &request.staged, &id)?;
    core::write_json(&core::safe_join(dir, "ready.json")?, &json!({"ready":true}))?;
    let start = Instant::now();
    loop {
        if core::safe_join(dir, "cancel.json")?.exists()
            || launcher.exited()?
            || start.elapsed() > Duration::from_secs(70)
        {
            transaction::recover(&executable, &id)?;
            return core::fail("cancelled", "更新已取消");
        }
        if core::safe_join(dir, "go.json")?.exists() {
            break;
        }
        std::thread::sleep(Duration::from_millis(25));
    }
    core::write_json(
        &core::safe_join(dir, "accepted.json")?,
        &json!({"accepted":true}),
    )?;
    let install = (|| {
        platform::wait_original(&handles, Duration::from_secs(60))?;
        // Check again after handoff in case another application instance started while waiting.
        if !platform::app_processes(&executable)?.is_empty() {
            return core::fail("file_in_use", "主程序被重新打开，取消替换");
        }
        transaction::replace(&mut journal)?;
        platform::independent_command(&executable)
            .current_dir(executable.parent().unwrap())
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()?;
        Ok(())
    })();
    if let Err(error) = install {
        transaction::recover(&executable, &id).map_err(|e| core::Error {
            code: "recovery_failed",
            message: format!("{error}; {e}"),
        })?;
        if platform::app_processes(&executable)?.is_empty() {
            let _ = platform::independent_command(&executable)
                .current_dir(executable.parent().unwrap())
                .spawn();
        }
        return Err(error);
    }
    // Once the OS starts the new process, journal failures must not roll back its executable.
    transaction::complete(&mut journal)?;
    // The installation journal owns its recovery copy; the download is now redundant.
    if let (Some(root), Some(stage_dir)) = (dir.parent(), request.staged.parent()) {
        if stage_dir.parent() == Some(root) {
            if let Some(id) = stage_dir.file_name().and_then(|name| name.to_str()) {
                cache_files::remove(root, id);
            }
        }
    }
    Ok(())
}
fn alert(error: &core::Error, localized: Option<&str>) {
    // The application supplies the text in its UI language; the code stays for diagnosis.
    let message = match localized {
        Some(text) => format!("{text}\n\n({})", error.code),
        None => format!("截图吧更新未完成：{error}\n已保留备份和恢复记录。"),
    };
    #[cfg(windows)]
    {
        let message: Vec<u16> = message
            .encode_utf16()
            .chain(Some(0))
            .collect();
        let title: Vec<u16> = "jietuba update".encode_utf16().chain(Some(0)).collect();
        unsafe {
            windows_sys::Win32::UI::WindowsAndMessaging::MessageBoxW(
                std::ptr::null_mut(),
                message.as_ptr(),
                title.as_ptr(),
                windows_sys::Win32::UI::WindowsAndMessaging::MB_OK
                    | windows_sys::Win32::UI::WindowsAndMessaging::MB_ICONERROR,
            );
        }
    }
    #[cfg(not(windows))]
    {
        eprintln!("{message}");
    }
}
fn run(command: &str, args: &HashMap<String, String>, id: &str) -> Result<()> {
    platform::sanitize_loader()?;
    if command == "--version" {
        return protocol::emit(
            "version",
            "",
            json!({"version":env!("CARGO_PKG_VERSION"),"test_build":false}),
        );
    }
    if command == "worker" {
        let dir = PathBuf::from(arg(args, "--worker-dir")?);
        let localized = fs::read(dir.join("request.json"))
            .ok()
            .and_then(|bytes| serde_json::from_slice::<WorkerRequest>(&bytes).ok())
            .and_then(|request| request.failure_message);
        let result = worker(&dir);
        let data = match &result {
            Ok(()) => json!({"event":"complete"}),
            Err(e) => json!({"event":"error","code":e.code,"message":e.message}),
        };
        let _ = core::write_json(&core::safe_join(&dir, "result.json")?, &data);
        if let Err(error) = &result {
            if dir.join("accepted.json").exists() {
                alert(error, localized.as_deref());
            }
        }
        return result;
    }
    let executable = transaction::install_exe(Path::new(arg(args, "--install-exe")?))?;
    match command {
        "check" => {
            protocol::emit("started", id, json!({}))?;
            let arch = platform::pe_arch(&executable)?;
            let release = runtime()?.block_on(download::fetch_latest(
                arch,
                variant(args)?,
                &AtomicBool::new(false),
            ))?;
            let available = contract::version(&release.tag_name)?
                > contract::version(arg(args, "--current-version")?)?;
            protocol::emit(
                if available { "available" } else { "up_to_date" },
                id,
                serde_json::to_value(release)?,
            )
        }
        "download" => {
            protocol::emit("started", id, json!({}))?;
            let flag = cancellable(id);
            let arch = platform::pe_arch(&executable)?;
            let variant = variant(args)?;
            let release: contract::Release =
                serde_json::from_slice(&fs::read(arg(args, "--release-file")?)?)?;
            let current = arg(args, "--current-version")?;
            if release.arch != arch
                || release.asset_name != variant.asset_name(&release.tag_name, arch)
                || contract::version(&release.tag_name)? <= contract::version(current)?
            {
                return core::fail("version", "下载版本或架构不匹配");
            }
            let root = cache(args)?;
            let dir = core::safe_join(&root, id)?;
            fs::create_dir(&dir)?;
            let mut temporary = cache_files::DownloadDirectory {
                root: &root,
                id,
                keep: false,
            };
            let zip = core::safe_join(&dir, "package.zip")?;
            runtime()?.block_on(download::download_file(
                &release.artifact,
                &zip,
                &flag,
                |received, total, source| {
                    protocol::emit(
                        "progress",
                        id,
                        json!({"received":received,"total":total,"source":source}),
                    )
                },
            ))?;
            if flag.load(Ordering::Acquire) {
                return core::fail("cancelled", "下载已取消");
            }
            let staged = core::safe_join(&dir, "app.exe")?;
            archive::extract_exe(&zip, &staged, arch, variant)?;
            core::write_json(
                &core::safe_join(&dir, "prepared.json")?,
                &Prepared {
                    executable,
                    current_version: current.into(),
                    release,
                    staged_hash: core::hash_file(&staged)?,
                },
            )?;
            let _ = fs::remove_file(zip);
            protocol::emit("downloaded", id, json!({}))?;
            temporary.keep = true;
            Ok(())
        }
        "apply" => apply(args, &executable, id),
        "recover" => {
            if !platform::app_processes(&executable)?.is_empty() {
                return core::fail("file_in_use", "恢复前请关闭主程序");
            }
            let _lock = transaction::lock(&executable)?;
            transaction::recover(&executable, id)?;
            protocol::emit("recovered", id, json!({}))
        }
        _ => core::fail("arguments", "未知命令"),
    }
}
fn main() {
    let (command, args) = match options() {
        Ok(o) => o,
        Err(e) => {
            let _ = protocol::emit("error", "", json!({"code":e.code,"message":e.message}));
            std::process::exit(1);
        }
    };
    let id = args
        .get("--transaction")
        .cloned()
        .unwrap_or_else(core::random_id);
    if let Err(error) = run(&command, &args, &id) {
        if command != "worker" {
            let _ = protocol::emit(
                "error",
                &id,
                json!({"code":error.code,"message":error.message}),
            );
        }
        std::process::exit(1);
    }
}
