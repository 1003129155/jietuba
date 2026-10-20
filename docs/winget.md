# WinGet 发布

从 2.1.8 开始，发布工作流会为现有 ZIP 生成 WinGet 清单，附在 Release 草稿的 `winget-manifests.zip` 中。生成清单不等于已上架；公共源安装须等微软仓库审核并同步完成。

拟提交的两个独立软件 ID（以微软审核结果为准）：

| 版本 | ID | 启动命令 |
| --- | --- | --- |
| 完整版 | `1003129155.Jietuba` | `jietuba` |
| 轻量版 | `1003129155.Jietuba.Lite` | `jietuba-lite` |

两者都提供 x64 和 ARM64，清单使用 ZIP + portable 方式，保持现有包名、根目录 EXE 和完整版 `models/` 布局。便携式 EXE 不强制要求代码签名，保留现有可选 SignPath 流程。先签名、打包，再计算哈希和生成清单；此后不要替换相同 URL 的发行包。

## 发布者操作

1. 按原有流程发布 `release-2.1.8` 标签，检查构建产物和 Release 草稿。生成清单无需增加令牌或密钥。
2. 公开正式 Release。草稿中的附件无法供微软匿名下载，不要提交草稿或测试包。
3. 下载并解压 `winget-manifests.zip`。在 PowerShell 中分别验证两个版本目录：

   ```powershell
   winget validate .\manifests\1\1003129155\Jietuba\2.1.8
   winget validate .\manifests\1\1003129155\Jietuba\Lite\2.1.8
   ```

4. 在 Windows Sandbox 等干净环境中，用微软仓库的 `Tools/SandboxTest.ps1` 分别安装测试两个目录。检查启动、完整版 OCR 模型加载、设置和历史数据、退出后升级、卸载，以及卸载后用户数据是否符合预期。还应检查程序内一键更新与 WinGet 安装版本记录是否一致；完成验证前，建议 WinGet 用户通过 `winget upgrade` 更新。不要同时运行两种版本。
5. Fork `microsoft/winget-pkgs`，把生成的 `manifests/` 内容放到对应目录。完整版和轻量版分开提交两个 PR，每个 PR 只包含一个 ID、一个版本。处理机器人检查和维护者反馈。后续版本也需要提交更新清单。
6. 审核、合并并同步到公共源后，确认 `winget search --id 1003129155.Jietuba -e --source winget` 能找到条目，再在下载说明中宣布可用。

上架后用户可以执行：

```powershell
winget install --id 1003129155.Jietuba -e --source winget
# 或选择轻量版
winget install --id 1003129155.Jietuba.Lite -e --source winget
```

升级前先退出截图吧，再执行对应 ID 的 `winget upgrade --id ... -e --source winget`。已有手动解压版不会因为上架而自动迁移为 WinGet 管理的安装。

## 本地重新生成

需要 Python 3.11 或更新版本，将同一正式版本的四个最终 ZIP 放入一个目录：

```powershell
python scripts/generate_winget.py --assets release-assets --output build/winget --archive build/winget-manifests.zip
python -m unittest discover -s scripts -p "test_generate_winget.py"
```

默认版本来自根目录 `VERSION`，也可以指定 `--version 2.1.8`。脚本检查全部四个 ZIP、根目录 EXE、完整版模型文件和 ZIP 完整性，然后按实际 ZIP 计算 SHA256。它不会执行软件、发布 Release、修改系统设置或提交 PR。应使用专用输出目录，避免旧版清单混入归档。

本地 `winget validate` 只检查清单；实际发行包的下载、安装、运行和升级测试仍需使用最终公开产物完成。

参考：[微软提交流程](https://learn.microsoft.com/windows/package-manager/package/repository)、[清单格式](https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.6.0/installer.md)。
