<div align="center">

<img src="svg/%E6%89%98%E7%9B%98.svg" width="128" alt="jietuba logo" />

# jietuba

**Screenshot, Screen Drawing, OCR, Pin, Translation & Clipboard Tool for Windows**

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/cpp.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/cpp.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Download for Windows](https://github.com/1003129155/jietuba/releases/latest) · [Build from Source](#source-setup) · [Development and Tests](#development)

[简体中文](README_zh-CN.md) | [繁體中文](README_zh-TW.md) | **English** | [日本語](README_JA.md) | [한국어](README_KO.md)

</div>

## Overview

jietuba is a free, open-source screenshot and productivity tool for Windows, bringing screenshot annotation, screen drawing, OCR, translation, pinning, recording and clipboard history together. Screenshots, pins and the clipboard are connected, so anything you capture is easy to find again and reuse.

## Screenshot ⇄ pin ⇄ clipboard, seamlessly connected

Send a capture to history and pin it back to the screen in one click. Keep annotating, extract text and copy it back to the clipboard: three tools working together in one smooth flow.

- **A full screenshot toolkit, a smooth experience**: snap to windows and controls, with more than a dozen annotation tools at hand, plus four-way scrolling capture, auto-scroll, multiple monitors, mixed DPI and HDR capture.
- **Pin anything**: keep what you need on screen in one click: text, images or files. Zoom, annotate, grab text and translate as you work.
- **Clipboard: record everything**: not just your own screenshots. Anything copied from any source goes into history. Organize it into groups and favorites, search, filter and drag multiple items out. Find important content whenever you need it, drag anything into the window you want, or pin it on screen.
- **Fast, fluid and light on memory**: deeply optimized for quick responses and smooth operation, even in demanding workflows on low-end PCs. Measured resident memory (working set) drops to around 3 MB when idle, and stays low under heavy use as well. No installation needed: unzip and run.

It is written in C++ (Qt6), with image processing, clipboard access, the PP-OCR engine and other core parts in Rust, and runs on Windows x86_64 and ARM64. The earlier Python (PySide6) implementation is archived on the [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) branch and no longer updated.

## Features

<table>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="Capture and annotation" src="https://github.com/user-attachments/assets/397d6a3a-51a6-4860-9d05-0aed2c7c0429" />
<p><b>Capture and annotation</b><br>Snaps to windows and controls; pen, arrow, number, mosaic, spotlight, watermark and more, all undoable.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="Clipboard history" src="https://github.com/user-attachments/assets/d34ff233-fe7f-49dd-89ae-c5978fa25b5a" />
<p><b>Clipboard history</b><br>Record everything. Pin anything. Group favorites, search, filter and drag multiple items out. Find what you copied when you need it.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="Scrolling capture" src="https://github.com/user-attachments/assets/a179b3e7-d201-4208-b518-81cecc4e9661" />
<p><b>Scrolling capture</b><br>Stitches in all four directions, with auto-scroll; scrolling back only repositions, and one click trims the rest.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="OCR" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />
<p><b>OCR</b><br>Windows 11 Snipping Tool OCR and PP-OCR, offline; wrapped lines are joined and punctuation tidied.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="Pins" src="https://github.com/user-attachments/assets/d03e74e7-576b-46eb-8aea-21cc96d6a020" />
<p><b>Pin anything</b><br>One click keeps it on screen, ready to use. Zoom, annotate, grab text and translate without breaking your flow.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="GIF and MP4 recording" src="https://github.com/user-attachments/assets/284f09b1-24b2-4569-9bc7-278c3bcd34da" />
<p><b>GIF / MP4 recording</b><br>Annotate while recording and trim GIFs in playback; MP4 with custom frame rate and bitrate, system audio and the pointer, no microphone.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<p><b>Translation</b><br>Translate selected text with a single shortcut. Works with DeepL, Google, Azure, Amazon, Baidu, Volcengine, Tencent Cloud, Youdao, DeepSeek and any OpenAI-compatible API, and reads results aloud.</p>
</td>
<td width="50%" valign="top">
<p><b>Screen drawing</b><br>Write and annotate directly on screen as you demonstrate. A whiteboard and cursor highlights make presentations and lessons easy to follow.</p>
</td>
</tr>
</table>

Also:

- **Code scanning**: reads several QR codes and barcodes in a selection at once.
- **Global mouse shortcuts**: hold modifiers and drag to capture, pin, recognize text, translate or scan QR codes / barcodes straight away.
- **And more**: PDF export, watermarks, delayed capture, five interface languages (Simplified and Traditional Chinese, English, Japanese, Korean).


---

## Download and Run

1. Open the [Releases page](https://github.com/1003129155/jietuba/releases/latest) and download the archive ending in `-x64.zip` or `-arm64.zip` for your device. No runtime needs installing.
2. Extract the entire archive and double-click the exe inside.

|  | Full `jietuba_pp-…zip` | Lite `jietuba_lite-…zip` |
|---|---|---|
| Contents | `jietuba_pp.exe` and the `models/` folder beside it (keep both) | A single `jietuba_lite.exe` |
| OCR engines | Snipping Tool OCR + bundled PP-OCR; recognition also works without the system Snipping Tool | Windows 11 Snipping Tool OCR by default; download the [PP-OCR pack](#ppocr-pack) in Settings if needed |
| Best for | Windows 10, or OCR that works out of the box | Windows 11 and a smaller download |

Both packages select an available engine automatically, preferring Windows 11 Snipping Tool OCR and otherwise trying PP-OCR; you can select an engine under **OCR Settings**. Supported languages depend on the engine and model; the bundled PP-OCRv6 small model supports Chinese, English and Japanese. Without the Snipping Tool, or with a version older than 11.2308 (no "Text actions"), Lite needs a working Snipping Tool OCR installation or the PP-OCR pack before it can recognize text.

> [!NOTE]
> Windows may display a warning after you download the program through a browser. If prompted, click **More info**, then **Run anyway**.

> [!WARNING]
> Both packages share settings and clipboard history, so you can switch between them, but do not run both at once: the default hotkeys conflict.

> [!TIP]
> Settings are kept in `%APPDATA%\Jietuba\settings.ini`. Put a `settings.ini` next to the exe (an empty file is enough) and the program uses that one instead, so copying the program folder takes the settings along. Settings that only make sense on this computer, such as the screenshot save folder, the clipboard history location and autostart, stay in the registry; the clipboard history is in `%APPDATA%\pyclipboard` by default and can be moved on the Clipboard page of the settings.

### Updates

After installing the first release containing the updater, use **About → Check for Updates → Update and Restart**. It downloads the matching GitHub Release ZIP, replaces only the application EXE, and restarts. Settings, clipboard history, models and other files remain intact. Finish recording, exporting and saving before installation. The last application backup is kept under `.jietuba-update/`; see [updater recovery](rust_libs/updater/README.md). Python builds older than 2.1.8 switch to the C++ build through the one-click update, keeping settings, clipboard history and models.

<a id="ppocr-pack"></a>

### PP-OCR Pack (Lite)

Lite includes PP-OCR's pre- and post-processing but not ONNX Runtime or models. Download them under **Manage PP-OCR Models** in Settings: models come from ModelScope; ONNX Runtime is extracted from Microsoft's `onnxruntime` wheel, using PyPI or a mirror. Downloads are checked against the sizes and hashes in the built-in catalog. Choose from PP-OCRv6, PP-OCRv5 and language-specific recognizers, or add a local ONNX file. Downloading, adding and switching need no restart.

You can also place the files by hand in the `models/` folder beside the exe: `PP-OCRv6_det_small.onnx` and `PP-OCRv6_rec_small.onnx` (both are in the repository's [models/](models/)), plus a CPU-only `onnxruntime.dll` of ONNX Runtime 1.24 or later.

## Code Signing Policy

The Windows release executables, `jietuba_pp.exe` and `jietuba_lite.exe`, are built by GitHub Actions from the C++ source in this repository's [cpp/](cpp/). The release workflow supports code signing through SignPath; signing depends on the build configuration. Check the EXE's digital signature and the notes for that release.

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- Committer, reviewer and approver: [@1003129155](https://github.com/1003129155)
- External contributions are submitted as pull requests and merged only after review by the maintainer.

<a id="privacy"></a>

### Privacy

Capture, OCR, annotation, recording and clipboard history are processed and stored on your computer. Every network feature requires a manual action from you; the app never connects automatically. These features include:

- Everyday capture, OCR recognition and clipboard recording do not require uploading your content. PP-OCR needs local models and ONNX Runtime before recognition.
- Translation sends the text to the service selected in Settings, using the credentials you configure; that service's privacy policy applies. A custom OpenAI-compatible endpoint can also point to a service on your own computer, in which case the text goes to that local endpoint.
- When you check for or install updates under **About → Check for Updates**, the program contacts GitHub to read release information and download the release package.
- Downloading models or ONNX Runtime in **Manage PP-OCR Models** contacts ModelScope, PyPI or mirrors listed in the built-in catalog. These requests download components; they do not upload images for recognition.
- Testing a translation connection or fetching a model list contacts the service endpoint you configure. Connection tests send fixed sample text; fetching a model list does not send clipboard content.

---

<a id="source-setup"></a>

## Build from Source

You need Visual Studio's "Desktop development with C++" workload (including CMake), the Rust MSVC toolchain and the Qt 6.11 MSVC SDK. In PowerShell, from `cpp/`:

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py   # downloads the Qt SDK into .sdk/
.\build.cmd                                         # builds, tests and deploys to dist/Release
```

If you already have a Qt 6 MSVC x64 SDK, use `.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64`; `-SkipTests` skips the tests and `-Configuration Debug` builds Debug. Run `run.cmd` when the build finishes.

The released single-file builds link Qt and the MSVC runtime statically, so neither needs a separate installation. OCR models and ONNX Runtime are described in the component-pack section above. Build a static Qt SDK from Qt's official sources with `.\scripts\build_static_qt.ps1`, then:

```powershell
.\build.cmd -Static          # Lite: dist/Release-static/jietuba_lite.exe
.\build.cmd -Static -Full    # Full: dist/Release-static-full/jietuba_pp.exe, embeds ONNX Runtime and ships the default PP-OCR models
```

The ARM64 build is cross-compiled on x64: build the x64 static Qt first, then run `.\scripts\build_static_qt.ps1 -Arch arm64` and `.\build.cmd -Static -Arch arm64`. It needs Visual Studio's MSVC ARM64 build tools and `rustup target add aarch64-pc-windows-msvc`. The version number comes from the repository's `VERSION` file.

The PP-OCR models are in the repository's [models/](models/) directory: `PP-OCRv6_det_small.onnx` and `PP-OCRv6_rec_small.onnx`. Snipping Tool OCR uses files from the local Windows 11 Snipping Tool, copied to `%LOCALAPPDATA%\Jietuba\oneocr` on first use; nothing else needs to be downloaded.

---

<a id="development"></a>

## Development and Tests

`.\build.cmd` builds and runs all the tests. Tests whose names end in `_desktop` inject real keyboard and mouse input, register global hotkeys and capture the screen, so leave the mouse and keyboard alone while they run. To run only the tests that need no desktop:

```powershell
$env:PATH = "$PWD\.sdk\6.11.0\msvc2022_64\bin;$env:PATH"
ctest --test-dir build\msvc -C Release --output-on-failure -E "_desktop$|_windows_parity$"
```

The test programs run in a temporary sandbox and never read or write your own settings, clipboard history or logs. There are also analysis builds (`scripts\check.ps1`: AddressSanitizer, code analysis, leak detection) and coverage (`scripts\coverage.ps1`).

[C++ CI](.github/workflows/cpp.yml) builds every pull request, runs offscreen tests and checks line coverage. Desktop tests run separately and their failures do not block that check. When `rust_libs/` changes, [Rust CI](.github/workflows/rust.yml) runs Rust unit tests and rechecks third-party notices.

Releases are made by [build.yml](.github/workflows/build.yml): the tag must be `release-<version in the VERSION file>`. [lite.yml](.github/workflows/lite.yml) builds Lite and Full for x64 and ARM64; the release workflow signs them when configured and packages them as `jietuba_lite-<tag>-<arch>.zip` and `jietuba_pp-<tag>-<arch>.zip`.

---

## Directory Structure

```text
# Project root
├── README.md / README_zh-CN.md / README_zh-TW.md / README_JA.md / README_KO.md   # English, Chinese, Japanese and Korean documentation
├── VERSION                  # Version number, read by CMake and the release workflow
├── app.ico                  # Application and updater icon
├── licenses/                # Third-party licenses shipped with the release packages
│
├── cpp/                     # C++ (Qt6) implementation
│   ├── src/                 # Source, organized as <package>/<module>
│   │   ├── annotation/      # Live screen annotation — one layer per screen, pass-through, cross-screen undo
│   │   ├── barcode/         # QR code and barcode recognition (zxing-cpp)
│   │   ├── canvas/          # Canvas — editing core, undo, layers
│   │   ├── capture/         # Capture — screenshots, window and element detection
│   │   ├── clipboard/       # Clipboard manager — history, groups/quick launch, import/export, search
│   │   ├── core/            # Core — bootstrap, logging, resources, theme, i18n, hotkeys
│   │   ├── gif/             # GIF/MP4 recording, GIF editing, playback, export
│   │   ├── ocr/             # Snipping Tool OCR and PP-OCR
│   │   ├── pin/             # Pins — text, images, files, annotation, OCR and translation
│   │   ├── settings/        # Unified settings
│   │   ├── stitch/          # Long screenshot stitching
│   │   ├── text_recognition/ # OCR result layout
│   │   ├── tools/           # Drawing tools — pen, rectangle, arrow, text, mosaic and more
│   │   ├── translation/     # Multi-provider translation and read-aloud
│   │   ├── ui/              # UI components, capture window, toolbar, settings window
│   │   └── video/           # MP4 recording worker
│   ├── native/              # C interface to rust_libs, linked statically
│   ├── resources/           # Default settings, UI translations (XML and .qm), log messages
│   ├── scripts/             # Build, Qt SDK setup, translation compiling
│   └── tests/               # QtTest tests and reference data
│
├── rust_libs/               # Rust libraries, linked statically into the C++ build through cpp/native
│   ├── gifrecorder/         # GIF frame storage, playback and encoding
│   ├── longstitch/          # Long screenshot stitching algorithm
│   ├── pyclipboard/         # Low-level clipboard operations
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX recognition engine
│   ├── hdrcapture/          # HDR desktop capture
│   ├── updater/             # Standalone updater that replaces only the application EXE
│   └── video_recorder/      # Media Foundation recording and WASAPI system audio capture
│
├── models/                  # PP-OCR ONNX models (shipped with the Full package)
│   ├── PP-OCRv6_det_small.onnx   # Text detection model (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # Text recognition model (CRNN/CTC)
│
└── svg/                     # SVG icons
```

## Module Details

Module-by-module details are in [docs/MODULES.md](docs/MODULES.md).

---

## License

Copyright and the MIT license are in [LICENSE](LICENSE); Qt and other third-party components are described in [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md).
