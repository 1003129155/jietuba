[中文](README_zh-CN.md) | **[English](README.md)** | [日本語](README_JA.md)

# jietuba — Screenshot, OCR, Pin, Translation & Clipboard Tool for Windows

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/ci.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Download for Windows](https://github.com/1003129155/jietuba/releases/latest) · [Run from Source](#source-setup) · [Development and Tests](#development)


<img width="1391" height="844" alt="21" src="https://github.com/user-attachments/assets/01770043-6dbe-4c9f-b1d9-5d9eb467c559" />

## Overview

jietuba is a free, open-source screenshot tool for Windows: region and window capture, scrolling (long) screenshots, annotation, OCR text recognition, translation, image pinning, GIF/MP4 recording, QR code and barcode scanning, PDF export, and a full clipboard history manager. Everything runs locally.

MP4 region recording supports custom frame rates and bitrates, pause/resume, the mouse pointer, and system audio. Audio from common 5.1/7.1 playback devices is converted to stereo; microphone recording is not included.

jietuba is written in C++ (Qt6); image processing, clipboard access, the PP-OCR engine and other core parts are implemented in Rust. Runs on Windows x86_64 and ARM64. The earlier Python (PySide6) implementation is archived on the [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) branch and no longer updated.

Download a ready-to-run Windows release or build it from source.

---

## Highlights

A smooth, three-way "screenshot ⇄ clipboard ⇄ pin" workflow.

- **Anything you copy lands in history** — not just your own screenshots: any text, image, file, or HTML you copy goes automatically into the same clipboard history, ready to group, save permanently, export in one click, and share across devices

- **History → pin → keep working** — pin any image from history back to the top of the screen with one click. Bring it up anytime to compare, zoom, keep annotating, or re-run OCR

- **Polished down to the details** — scroll capture stitches precisely in all four directions within milliseconds; seamless capture across multiple monitors and mixed DPI; export images in various encodings. Every feature rivals paid software — or does even better

- **Memory footprint** — a strict runtime memory design keeps resident memory very low even under continuous, complex usage (often just a dozen MB or even a few MB)

- **Smooth UI** — demanding scenarios are optimized and CPU usage reduced, so even low-end machines run smoothly

- **Data safety** — everything runs entirely on your machine: no data collection, no silent network calls, all data stays local. Translation is the one feature that needs the internet, and only when you actively use it, calling whichever third-party translation API you've chosen

---

## Download and Run

The Windows x86_64 and ARM64 releases are ready to run. You do not need to install any runtime or development environment.

1. Open the [Releases page](https://github.com/1003129155/jietuba/releases/latest) and download the archive ending in `-x64.zip` or `-arm64.zip` for your device.
2. There are two packages. Extract the entire archive and double-click the exe inside:
   - **Full** `jietuba_pp-…zip`: includes the PP-OCR engine and models, so OCR works on any Windows. Keep `jietuba_pp.exe` and the `models/` folder together.
   - **Lite** `jietuba_lite-…zip`: a single exe without PP-OCR, smaller to download. By default it uses only the OCR built into the Windows 11 Snipping Tool; OCR is unavailable on PCs without it, or where it is older than 11.2308 (no "Text actions"). Update the Snipping Tool or use the Full package. Placing the PP-OCR pack beside the exe also enables PP-OCR; see [cpp/README.md](cpp/README.md#pp-ocr-组件包) (in Chinese).
3. Both packages prefer the Snipping Tool OCR by default (faster, more languages). Switch engines under **OCR Settings** in Settings.
4. The application is not digitally signed, so Windows may display a warning after you download it through a browser. If prompted, click **More info**, then **Run anyway** to start the application.

Both packages share settings and clipboard history, so you can switch between them, but do not run both at once (the default hotkeys conflict).

After installing the first release containing the updater, use **About → Check for Updates → Update and Restart**. It downloads the matching GitHub Release ZIP, replaces only the application EXE, and restarts. Settings, clipboard history, models and other files remain intact. Finish recording, exporting and saving before installation. The last application backup is kept under `.jietuba-update/`; see [updater recovery](rust_libs/updater/README.md). Python builds older than 2.1.8 switch to the C++ build through the one-click update, keeping settings, clipboard history and models.

## Code Signing Policy

The `jietuba_pp.exe` and `jietuba_lite.exe` files in the Windows releases are both built by GitHub Actions from the C++ source in this repository's [cpp/](cpp/) and signed through SignPath.

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- Committer, reviewer and approver: [@1003129155](https://github.com/1003129155)
- External contributions are submitted as pull requests and merged only after review by the maintainer.

### Privacy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

- Screenshots, OCR and clipboard history are processed and stored on your computer.
- When you translate, the text is sent to the translation service selected in Settings (DeepL, Google, Azure, Amazon, Baidu, Volcengine, Tencent Cloud, Youdao, DeepSeek, or an OpenAI-compatible endpoint you configure) using your own API key; that service's privacy policy applies.
- When you check for or install updates under **About → Check for Updates**, the program contacts GitHub to read release information and download the release package.

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

`.\build.cmd -Static` builds the single-file Lite exe used for releases, and `-Static -Full` the Full package. Preparing the static Qt, the PP-OCR pack and the rest are covered in [cpp/README.md](cpp/README.md) (in Chinese).

The PP-OCR models are in the repository's [models/](models/) directory: `PP-OCRv6_det_small.onnx` and `PP-OCRv6_rec_small.onnx`. Snipping Tool OCR uses files from the local Windows 11 Snipping Tool, copied to `%LOCALAPPDATA%\Jietuba\oneocr` on first use; nothing else needs to be downloaded.

### Rust Extension Packages

The seven libraries in [rust_libs/](rust_libs/) are also published to PyPI as standalone Python extension packages. jietuba itself links their Rust cores directly from C++ and does not depend on these packages. Their PyPI distribution and Python import names are:

| pip name | import name | Version | Description |
|------|------|------|------|
| [`j-gif`](https://pypi.org/project/j-gif/) | `gifrecorder` | 0.4.1 | GIF/video composition encoder |
| [`j-stitch`](https://pypi.org/project/j-stitch/) | `longstitch` | 0.5.1 | Long screenshot stitching algorithm |
| [`j-clipboard`](https://pypi.org/project/j-clipboard/) | `pyclipboard` | 0.4.7 | Low-level clipboard operations |
| [`j-ppocr`](https://pypi.org/project/j-ppocr/) | `ppocr_rust` | 0.3.0 | PP-OCR (PaddleOCR) ONNX text recognition (pure Rust + ONNX Runtime, needs det/rec models) |
| [`j-hdrcapture`](https://pypi.org/project/j-hdrcapture/) | `hdrcapture` | 0.1.0 | HDR-correct desktop capture (DXGI Desktop Duplication + GPU tone mapping) |
| [`j-input`](https://pypi.org/project/j-input/) | `inputhub` | 0.1.2 | Global mouse and keyboard hooks off the GUI thread (gestures, side buttons, hotkeys, wheel, foreground window) |
| [`j-video`](https://pypi.org/project/j-video/) | `video_recorder` | 0.1.0 | Media Foundation H.264/MP4 recording with WASAPI system audio |

The seven extensions target Windows x86_64 and ARM64, declare Python `>=3.11`, and enable `abi3-py311`. See each package's `pyproject.toml` and `Cargo.toml` for its configuration.

---

<a id="development"></a>

## Development and Tests

Building, testing and development conventions are described in [cpp/README.md](cpp/README.md). [C++ CI](.github/workflows/cpp.yml) builds every pull request and runs the tests that need no desktop; when `rust_libs/` changes, [Rust CI](.github/workflows/rust-wheels.yml) runs the Rust unit tests, rechecks third-party notices and builds the wheels. Desktop tests that inject real input, register hotkeys or capture the screen run on developer machines only.

The release workflow is [build.yml](.github/workflows/build.yml): the tag must be `release-<version in the VERSION file>`; [lite.yml](.github/workflows/lite.yml) builds the Lite and Full packages for x64 and ARM64, which are signed and published as `jietuba_lite-<tag>-<arch>.zip` and `jietuba_pp-<tag>-<arch>.zip`.

### C++ Implementation

Since 2.1.8 C++ ([cpp/](cpp/)) is the only implementation, and both released packages are built from it as single static exes: Lite `jietuba_lite.exe` (`build.ps1 -Static`); Full `jietuba_pp.exe` (`-Static -Full`) additionally embeds ONNX Runtime, and its package carries the default PP-OCR models in `models/`. Older updaters match package names exactly and replace only the same-named exe at the zip root, leaving `models/` in place, so package and exe names must not change; the Full exe puts `onnxruntime.dll` into `models/` at startup. The version number lives only in the repository's `VERSION` file.

The Python (PySide6) version is archived on the [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) branch and no longer updated. The pure-logic fixtures in the C++ tests were generated from it, are committed and are not regenerated; new features are implemented in C++ with expected values written in the C++ tests.

### Rust Library Layering

The logic in `rust_libs/` is written once and shared by the C++ version and the Python extensions published to PyPI:

- Each library has two layers: the core is pure Rust; pyo3 bindings live only in `python.rs` (`bindings.rs` in the recorder), behind the default `python` feature. The C++ version depends on the crates with `default-features = false` and calls the same core through its own C interface (`cpp/native/`); new capabilities only need to be wired into `cpp/native/`, without new pyo3 bindings.
- Bindings only convert types, take and release the GIL, map errors and manage object lifetimes; decisions, defaults, retries and threading belong in the core.
- Recoverable errors in the core return `Result` instead of panicking.
- Non-UI logic that does not need Qt goes into the Rust core first; UI code that uses Qt types stays in C++.

### Code Comments

Comments explain only the constraints and design reasons that the code cannot express. Keep them short and precise:

- No debugging stories, change history, incident write-ups, or development-time reasoning.
- Do not mention other software or projects, or cite them as justification.
- No essay-length explanations; do not repeat what the code, function names, or test names already say.

---

## Directory Structure

```text
# Project root
├── README.md / README_zh-CN.md / README_JA.md   # English, Chinese and Japanese documentation
├── VERSION                  # Version number, read by CMake and the release workflow
├── 托盘.ico                 # Application and updater icon
├── licenses/                # Third-party licenses shipped with the release packages
│
├── cpp/                     # C++ (Qt6) implementation
│   ├── src/                 # Source, organized as <package>/<module>
│   │   ├── barcode/         # QR code and barcode recognition (zxing-cpp)
│   │   ├── canvas/          # Canvas — editing core, undo, layers
│   │   ├── capture/         # Capture — screenshots, window and element detection
│   │   ├── clipboard/       # Clipboard manager — history, groups/quick launch, import/export, search
│   │   ├── core/            # Core — bootstrap, logging, resources, theme, i18n, hotkeys
│   │   ├── gif/             # GIF/MP4 recording, GIF editing, playback, export
│   │   ├── ocr/             # Snipping Tool OCR and PP-OCR
│   │   ├── pin/             # Pinned images — always on top, editing, OCR, translation
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
├── rust_libs/               # Rust libraries, shared by the C++ implementation and the PyPI extensions
│   ├── gifrecorder/         # GIF/video composition encoder
│   ├── longstitch/          # Long screenshot stitching algorithm
│   ├── pyclipboard/         # Low-level clipboard operations
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX recognition engine
│   ├── hdrcapture/          # HDR desktop capture
│   ├── inputhub/            # Global mouse and keyboard hooks
│   ├── updater/             # Standalone updater that replaces only the application EXE
│   └── video_recorder/      # Media Foundation recording and WASAPI system audio capture
│
├── models/                  # PP-OCR ONNX models (shipped with the Full package)
│   ├── PP-OCRv6_det_small.onnx   # Text detection model (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # Text recognition model (CRNN/CTC)
│
└── svg/                     # SVG icons
```
