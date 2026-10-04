# 独立 Rust 更新器

支持 Windows x64 / ARM64，按主程序通过 `--variant` 报告的版本类型选择 GitHub Release ZIP：`full` 取完整版包里的 `jietuba_pp.exe`，`lite` 取轻量版包里的 `jietuba_lite.exe`。只提取对应 EXE 并原地替换当前主程序，主程序改过文件名也照常更新；不修改配置、剪贴板历史、模型或其他文件。
版本类型由打包脚本写入程序内的 `updater/app_variant.txt`。缺少对应下载包时停止更新，不切换版本类型。
校验 ZIP 可读、根目录 EXE 唯一、PE 格式有效且匹配已安装应用的架构；不要求上游提供清单、签名或哈希附件。

## 构建

```powershell
cargo rustc --locked --release --target x86_64-pc-windows-msvc --manifest-path rust_libs/Cargo.toml -p jietuba-updater --bin jietuba_updater -- -C target-feature=+crt-static
```

以上命令在仓库根目录执行。ARM64 将目标改为 `aarch64-pc-windows-msvc`。
`build_with_ocr_onefile.py` 自动构建并内嵌更新器和许可资源，不参与 maturin 或 PyPI 发布。
应用版本使用 Python `APP_VERSION`，更新器 crate 版本独立维护。

依赖变化后使用 `cargo about generate --manifest-path rust_libs/updater/Cargo.toml -o rust_libs/updater/THIRD-PARTY-NOTICES.txt rust_libs/updater/notices.hbs` 更新第三方许可。

## 命令与协议

- `check --install-exe PATH --current-version VERSION --variant full|lite`：检查新版本。
- `download --install-exe PATH --current-version VERSION --variant full|lite --release-file PATH`：下载检查结果中固定的 Release 和资产。
- `apply --install-exe PATH --current-version VERSION --variant full|lite --transaction ID --parent-pid PID [--failure-message TEXT]`：准备安装并等待应用授权退出。`--failure-message` 是交接后失败时原生提示的正文，由应用按界面语言传入。
- `recover --install-exe PATH --transaction ID`：关闭应用后恢复指定事务。

`--transaction ID` 为 32 位小写十六进制。缓存默认位于 `%LOCALAPPDATA%/jietuba/updater`，可通过 `--cache-dir PATH` 指定绝对路径。

出错时 `error` 事件的 `code` 是稳定的错误类别，应用据此显示界面语言的提示；`message` 为中文诊断信息，只写日志。

标准输出使用 JSONL：`{"protocol":1,"event":"progress","transaction":"...","data":{"received":0,"total":123,"source":"..."}}`。
标准输入接受 `{"protocol":1,"transaction":"...","command":"cancel"}` 或 `go`。
收到 `ready` 后才能发送 `go`；交接前断连视为取消。收到 `handed_off` 后应用正常退出，worker 独立等待已确认的应用及 onefile 父进程退出，最多 60 秒，不强杀。

`DownloadArtifact` / `download_file` 支持候选源、重试、进度和取消；`ReleaseSource`、`ModelArtifact` 预留镜像和独立模型下载接口，当前仅启用 GitHub。

## 恢复与限制

安装目录的 `.jietuba-update/ID/` 保存备份、暂存 EXE 和 `journal.json`，保留最近一次备份。
系统无法启动新版时自动恢复并启动旧版；进程启动成功不代表业务初始化成功，启动后的故障可手动执行 `recover`，ID 取自事务文件夹名。
文件占用、外部修改或备份损坏时停止恢复并保留记录。交接后的失败写入缓存 worker 目录的 `result.json`，并显示原生提示。
重启时清理旧 PyInstaller 环境，使新版重新解包。
