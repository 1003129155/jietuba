<div align="center">

<img src="svg/%E6%89%98%E7%9B%98.svg" width="128" alt="jietuba logo" />

# jietuba

**Screenshot, OCR, Pin, Translation & Clipboard Tool for Windows**

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/cpp.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/cpp.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Download for Windows](https://github.com/1003129155/jietuba/releases/latest) · [Build from Source](#source-setup) · [Development and Tests](#development)

[中文](README_zh-CN.md) | **English** | [日本語](README_JA.md)

</div>

## Overview

jietuba is a free, open-source screenshot tool for Windows: capture, annotation, OCR, translation, pinning, screen recording and clipboard history in one program, all running locally.

It is written in C++ (Qt6), with image processing, clipboard access, the PP-OCR engine and other core parts in Rust, and runs on Windows x86_64 and ARM64. The earlier Python (PySide6) implementation is archived on the [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) branch and no longer updated.

## Features

<table>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="Capture and annotation" src="https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827" />
<p><b>Capture and annotation</b><br>Snaps to windows and controls; pen, arrow, number, mosaic, spotlight, watermark and more, all undoable.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="Clipboard history" src="https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a" />
<p><b>Clipboard history</b><br>Text, images and files you copy are kept and can be grouped for good; open it with Win+V, select several, drag them out or pin one to the screen.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="Scrolling capture" src="https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a" />
<p><b>Scrolling capture</b><br>Stitches in all four directions, with auto-scroll; scrolling back only repositions, and one click trims the rest.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="OCR" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />
<p><b>OCR</b><br>Windows 11 Snipping Tool OCR and PP-OCR, offline; wrapped lines are joined and punctuation tidied.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="Pins" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />
<p><b>Pins</b><br>Keep a screenshot on top to zoom, rotate and annotate; select its text to copy or translate, or turn it into editable text.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="GIF and MP4 recording" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
<p><b>GIF / MP4 recording</b><br>Annotate while recording and trim GIFs in playback; MP4 with custom frame rate and bitrate, system audio and the pointer, no microphone.</p>
</td>
</tr>
</table>

Also:

- **Live screen annotation**: draw on the screen you are working on while the mouse keeps working the windows below until you pick a tool, made for demos and teaching; with a whiteboard and a cursor highlight (a glowing pointer and click rings). Start it from Screen Annotation in the tray menu or its hotkey under Settings → Shortcuts → Screen Annotation.
- **Translation**: translate selected text with one key through DeepL, Google, Azure, Amazon, Baidu, Volcengine, Tencent Cloud, Youdao, DeepSeek or any OpenAI-compatible API, and have the result read aloud.
- **Code scanning**: reads several QR codes and barcodes in a selection at once.
- **Global mouse shortcuts**: hold modifiers and drag to capture, pin, recognize text, translate or scan QR codes / barcodes straight away.
- **And more**: PDF export, watermarks, delayed capture, five interface languages (Simplified and Traditional Chinese, English, Japanese, Korean).

## Highlights

A smooth, three-way "screenshot ⇄ clipboard ⇄ pin" workflow.

- **Anything you copy lands in history**: not just your own screenshots; any text, image, file or HTML you copy goes into the same clipboard history, ready to group, keep for good and export to other devices in one click.
- **History → pin → keep working**: pin any image from history back to the top of the screen with one click, to compare, zoom, keep annotating or re-run OCR.
- **Polished down to the details**: scroll capture stitches precisely in all four directions within milliseconds; seamless capture across multiple monitors and mixed DPI; export images in various encodings. Every feature rivals paid software, or does better.
- **Low memory**: a strict memory design keeps resident memory very low even after long, heavy use (often a dozen MB or even a few).
- **Smooth UI**: demanding scenarios are optimized and CPU usage reduced, so even low-end machines run smoothly.
- **Data safety**: everything runs on your machine, with no data collection and no silent network calls; translation is the one feature that goes online, and only when you use it, through the translation API you chose.

---

## Download and Run

1. Open the [Releases page](https://github.com/1003129155/jietuba/releases/latest) and download the archive ending in `-x64.zip` or `-arm64.zip` for your device. No runtime needs installing.
2. Extract the entire archive and double-click the exe inside.

|  | Full `jietuba_pp-…zip` | Lite `jietuba_lite-…zip` |
|---|---|---|
| Contents | `jietuba_pp.exe` and the `models/` folder beside it (keep both) | A single `jietuba_lite.exe` |
| OCR engines | Snipping Tool OCR + bundled PP-OCR, works on any Windows | Only the Windows 11 Snipping Tool OCR by default; the [PP-OCR pack](#ppocr-pack) can be downloaded in Settings |
| Best for | Windows 10, or OCR that works out of the box | Windows 11 and a smaller download |

Both packages prefer the Snipping Tool OCR by default (faster, more languages); switch engines under **OCR Settings** in Settings. On PCs without the Snipping Tool, or where it is older than 11.2308 (no "Text actions"), the Lite package needs an updated Snipping Tool or the PP-OCR pack before it can recognize text.

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

The Lite package includes PP-OCR's pre- and post-processing but not ONNX Runtime or models. The easiest way is to download them under **Manage PP-OCR Models** in Settings: ONNX Runtime comes from Microsoft's `onnxruntime` wheel on PyPI and is hash-checked after download; the models include PP-OCRv6, PP-OCRv5 and several language-specific recognizers, and you can also add a local ONNX file. Downloading, adding and switching need no restart.

You can also place the files by hand in the `models/` folder beside the exe: `PP-OCRv6_det_small.onnx` and `PP-OCRv6_rec_small.onnx` (both are in the repository's [models/](models/)), plus a CPU-only `onnxruntime.dll` of ONNX Runtime 1.24 or later.

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

If you already have a Qt 6 MSVC x64 SDK, use `.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64`; `-SkipTests` skips the tests and `-Configuration Debug` builds Debug. Run `run.cmd` when the build finishes.

The released single-file builds link Qt and the MSVC runtime statically and depend only on system DLLs. Build a static Qt SDK from Qt's official sources with `.\scripts\build_static_qt.ps1`, then:

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

[C++ CI](.github/workflows/cpp.yml) builds every pull request, runs the tests and checks the line coverage of the changed code; when `rust_libs/` changes, [Rust CI](.github/workflows/rust.yml) runs the Rust unit tests and rechecks third-party notices.

Releases are made by [build.yml](.github/workflows/build.yml): the tag must be `release-<version in the VERSION file>`; [lite.yml](.github/workflows/lite.yml) builds the Lite and Full packages for x64 and ARM64, which are signed and published as `jietuba_lite-<tag>-<arch>.zip` and `jietuba_pp-<tag>-<arch>.zip`.

---

## Directory Structure

```text
# Project root
├── README.md / README_zh-CN.md / README_JA.md   # English, Chinese and Japanese documentation
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
├── rust_libs/               # Rust libraries, linked statically into the C++ build through cpp/native
│   ├── gifrecorder/         # GIF/video composition encoder
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

### annotation/ — Screen Annotation Module

Draws straight on the screen being used. Every screen gets a full-screen transparent layer that reuses the GIF recording's drawing layer (`GifDrawingView`) and the capture toolbar (`Toolbar`, layout profile `toolbar_layout::ANNOTATION`), so tools, settings panels and undo are the same ones.

<details>
<summary>Show directory structure</summary>

```text
annotation/
├── screen_annotation.h/.cpp   # ScreenAnnotation — the session: a layer per screen, pass-through/drawing/hidden, cross-screen undo, focus hand-back; ScreenAnnotationShortcutHandler — keys while drawing
├── annotation_view.h/.cpp     # AnnotationView — one screen's drawing layer; as the canvas host it hands the shared toolbar to CanvasView
├── annotation_toolbar.h/.cpp  # AnnotationToolbar — floating toolbar with pass-through, clear, hide and exit; never takes the focus, kept out of captures and recordings
└── cursor_highlight.h/.cpp    # CursorHighlight and PointerLight — the light around the pointer and the click rings
```

</details>

**Key features:**
- In pass-through the layers carry `WS_EX_TRANSPARENT`, so the mouse reaches the windows below; picking a tool takes the mouse, and returning to pass-through gives the focus back to the previous foreground window
- One canvas and undo stack per screen; the session records which screens each step changed, so undo and redo follow the drawing order across screens, and clearing is one undoable step
- Layers are added, removed or resized as screens are plugged in or change resolution, and the toolbar always stays on a visible screen
- Before a capture the session returns to pass-through and waits for that frame to reach the screen: captures include the drawings but not the hit layer, and the toolbar is kept out of captures and recordings with `WDA_EXCLUDEFROMCAPTURE`
- The layers have no captured image under the drawings, so the highlighter cannot multiply onto what it covers; it is laid on at 35% opacity instead, each stroke in one pass so it keeps one tint, and a rectangle highlight fills its border and inside as one shape
- A shape just drawn is not selected, so no handles show on the screen or in recordings; clicking a drawing with its tool still selects it
- The whiteboard is a solid white item under the drawings, on and off with one click; pass-through shows the desktop again and the next tool brings the board back
- The cursor highlight takes the pointer and the buttons from the input hub's mouse events (the system hook), so it works in pass-through too, runs nothing while the pointer rests and misses no click; while drawing, its disc moves in the same repaint as the stroke, and a screenshot leaves it out

---

### barcode/ — Barcode Module

Scans QR codes and barcodes in the screenshot selection. The result window outlines each code on the screenshot and lists the decoded contents by number.

<details>
<summary>Expand directory structure</summary>

```text
barcode/
├── reader.h/.cpp         # read_codes / DecodedCode — zxing-cpp decoding; text, format and outline in reading order
├── result_window.h/.cpp  # BarcodeResultWindow — result window; screenshot and result list share the same numbers
└── barcode.h             # show_barcode_result — scan entry: decode the codes in the selection and open the result window (toolbar scan button)
```

</details>

- Supports QR Code, Data Matrix, Aztec, PDF417 and common linear barcodes such as Code 128 and EAN/UPC
- Reads every code in the selection at once; hovering a code on either side highlights it on both
- One-click copy; http(s) links open directly in the browser

---

### canvas/ — Canvas Module

Graphics editing canvas system with scene management, view rendering, item selection, and undo/redo.

<details>
<summary>Expand directory structure</summary>

```text
canvas/
├── scene.h/.cpp                  # CanvasScene — canvas scene, extends QGraphicsScene
├── view.h/.cpp                   # CanvasView — canvas view, extends QGraphicsView
├── selection_model.h/.cpp        # SelectionModel — manages selected graphics items
├── undo.h/.cpp                   # CommandUndoStack — undo/redo stack (add, delete, batch, edit commands)
├── smart_edit_controller.h/.cpp  # SmartEditController — handles selection/edit mode switching
├── smart_selection_anim.h/.cpp   # SmartSelectionAnimator — tweens the selection when it hops between windows
├── handle_editor.h/.cpp          # LayerEditor / EditHandle — control point drag editing
├── gestures.h/.cpp               # Mouse gesture state machines — text edge drag, rubber-band select, pending click-to-edit
├── handle_overlay.h/.cpp         # HandleOverlayWidget — separate compositing layer for edit handles, avoids full-scene repaint
├── canvas_host.h/.cpp            # CanvasHost — what a window hosting the canvas view offers it (toolbar, magnifier …), shared by the screenshot and pin windows
└── items/
    ├── drawing_items.h/.cpp      # RectItem / EllipseItem / NumberItem, and the DrawingItemMixin all drawn items share
    ├── stroke_item.h/.cpp        # StrokeItem — pen/highlighter stroke; repaints only the chunks that changed or are exposed
    ├── background_item.h/.cpp    # BackgroundItem — selection area background
    ├── mosaic_item.h/.cpp        # MosaicItem — pixel mosaic item
    ├── spotlight_item.h/.cpp     # SpotlightItem / SpotlightCurtain — spotlight holes and their shared curtain
    ├── selection_item.h/.cpp     # SelectionItem — selection boundary display
    ├── arrow_item.h/.cpp         # ArrowItem — arrow item, geometry for nine shaft and head styles
    ├── text_item.h/.cpp          # TextItem — text item, outline/shadow/background and tri-state interaction frame
    ├── watermark_item.h/.cpp     # WatermarkItem / WatermarkStyle — watermark layer over the selection: tiled, a diamond grid, or at one of seven positions, multi-line, time stamps
    └── item_reference.h          # ItemReference / ItemLease — item ownership and lifetime: the scene owns attached items, undo commands own detached ones
```

</details>

**Core Features:**
- Canvas system built on the Qt Graphics View Framework
- Freehand strokes, rectangles, ellipses, arrows, text, numbered markers and more
- Full undo/redo (based on QUndoStack)
- The smart edit controller switches seamlessly between selecting and drawing
- The handle editor supports transforming items

---

### capture/ — Capture Module

Screen capture and smart window detection.

<details>
<summary>Expand directory structure</summary>

```text
capture/
├── capture_service.h/.cpp           # CaptureService — core screenshot logic
├── display_watcher.h/.cpp           # DisplayChangeWatcher — rebuilds the HDR capture session in the background after display changes
├── system_cursor.h/.cpp             # SystemCursor — snapshots the mouse pointer and draws it into screenshots
├── quick_capture_controller.h/.cpp  # QuickCaptureController — modifier-drag capture and action dispatch
├── window_finder.h/.cpp             # WindowFinder — smart window selection, cursor-based detection
└── msaa_element_finder.h/.cpp       # MSAAElementFinder — point query: background threads ask MSAA for the control under the pointer and its parent chain, for smart selection
```

</details>

**Core Features:**
- Full-screen and region capture
- Smart window detection and selection (window shadows are excluded automatically)
- Window detection at the cursor position
- Point-based detection of the control under the pointer (MSAA): smart selection snaps level by level and shows a selection from the first frame

---

### clipboard/ — Clipboard Management Module

Clipboard history manager organized into controllers, core, services, and ui layers.
Supports text, images, HTML, files, a dedicated three-pane management window, and pin creation from history items.

<details>
<summary>Expand directory structure</summary>

```text
clipboard/
├── controllers/                        # Control layer — history loading, paste flow, menus, selection state
│   ├── clipboard_controller.h/.cpp     # ClipboardController — loading, pasting, context menu logic
│   ├── selection_manager.h/.cpp        # SelectionManager — list selection state
│   ├── context_menu_controller.h/.cpp  # ClipboardContextMenuController — assembles context menu data and actions
│   ├── foreground_tracker.h/.cpp       # ForegroundWindowTracker — remembers the window to paste into
│   └── paste_keystroke.h/.cpp          # Restores focus to the target window, then sends Ctrl+V
├── core/                               # Data layer — pyclipboard wrapper, models, group types
│   ├── manager.h/.cpp                  # ClipboardManager — storage, monitoring, and paste API
│   ├── models.h/.cpp                   # ClipboardItem / Group models
│   └── text_transform.h/.cpp           # Plain-text transforms — pure functions behind "paste special"
├── services/                           # Service layer — file payloads, group rules, import/export, save logic
│   ├── file_payload_service.h/.cpp     # file payload JSON + legacy format compatibility
│   ├── group_service.h/.cpp            # group icon/name/delete helpers
│   ├── import_export_service.h/.cpp    # CSV import/export for text items
│   └── manage_dialog_service.h/.cpp    # persistence logic for the management window
└── ui/
    ├── layout_scale.h/.cpp             # Shared size metrics for the management dialog
    ├── image_item_actions.h/.cpp       # load and save-as for image items, shared by both windows
    ├── group_kind_style.h/.cpp         # Identifying colours and line icons of the group types, shared by the sidebar rows and the type cards
    ├── dialogs/
    │   └── manage_dialog.h/.cpp        # three-pane management window for groups and content
    ├── forms/
    │   ├── group_form.h/.cpp
    │   ├── text_content_form.h/.cpp
    │   ├── file_content_form.h/.cpp
    │   ├── import_export_form.h/.cpp
    │   ├── group_icon_picker.h/.cpp
    │   ├── form_widgets.h/.cpp
    │   └── image_content_form.h/.cpp
    ├── menus/
    │   └── action_menu.h/.cpp
    ├── mixins/
    │   └── frameless_mixin.h/.cpp
    ├── panels/
    │   └── setting_panel.h/.cpp
    ├── resources/
    │   └── emoji_data.h/.cpp
    ├── theme/
    │   ├── themes.h/.cpp
    │   ├── theme_styles.h/.cpp
    │   └── chevron.h/.cpp              # Drop-down chevron images drawn per colour and size and cached for the style sheets
    ├── widgets/
    │   ├── group_bar.h/.cpp
    │   ├── item_delegate.h/.cpp
    │   ├── item_drag.h/.cpp            # Dragging history items out: text, HTML, images and files, written only when a target asks for files
    │   ├── selection_bar.h/.cpp        # The bar under the list during multiple selection: count, paste, save and other actions
    │   ├── preview_popup.h/.cpp
    │   ├── quick_edit_popup.h/.cpp
    │   ├── manage_rows.h/.cpp
    │   └── reorder_list.h/.cpp
    └── windows/
        ├── clipboard_window.h/.cpp     # history window, search, preview, quick paste
        └── pin_window.h/.cpp           # create pins from history items
```

</details>

**Core Features:**
- Monitor clipboard changes and store history automatically
- Support text, images, HTML, and files
- Support general groups, quick-launch groups, filter groups (always show one type of history), favorites, and search
- Dedicated three-pane management window for editing groups, text items, and file items
- CSV import/export for text items
- Multiple themes (light, dark and more)
- Quick paste of history items with shortcuts
- Select several items to paste, save or act on them together; drag items straight into editors, chat boxes or Explorer
- Change the data location in the settings: move the current data there, or switch to the data already in that folder
- Preview popups for large images and long text
- Opens with Win+V in place of the Windows clipboard history by default (the third clipboard hotkey slot); clear that slot to give Win+V back to Windows

---

### core/ — Core Module

Logging, resource loading, theme management, i18n, hotkeys, and other infrastructure.

<details>
<summary>Expand directory structure</summary>

```text
core/
├── bootstrap.h/.cpp            # PreloadManager — startup bootstrap, env init, DPI, single instance
├── data_location.h/.cpp        # Where the settings file lives: settings.ini next to the exe when there is one, else %APPDATA%\Jietuba
├── game_mode_monitor.h/.cpp    # GameModeMonitor — follows foreground switches to tell when a game is in front, for "Pause Hotkeys While Gaming"
├── elevation.h/.cpp            # Run as administrator — elevation state, scheduled task for sign-in
├── logger.h/.cpp               # Logger — file + console logging (debug/info/warning/error/exception)
├── crash_handler.h/.cpp        # install_crash_hooks() — global exception catching
├── resource_manager.h/.cpp     # ResourceManager — SVG/image resource loading
├── theme.h/.cpp                # ThemeManager — application theme colors
├── ui_scale.h/.cpp             # UIScaleManager — one scale factor for toolbars, panels and popups
├── i18n.h/.cpp                 # I18nManager / XmlTranslator / tr() — internationalization
├── shortcut_manager.h/.cpp     # HotkeySystem / ShortcutManager — global & in-app hotkeys
├── global_hotkey.h/.cpp        # Global hotkey key names and routing: system registration, hook takeover, or rejected
├── input_hub.h/.cpp            # Shared global input: hooks off the GUI thread, events as Qt signals
├── input_engine.h/.cpp         # Input state machine — gestures, hotkeys, shortcut recording, watchers
├── input_hooks.h/.cpp          # Low-level mouse/keyboard and foreground hooks on their own thread
├── quick_capture_input.h/.cpp  # Global mouse shortcut drag input on the shared input hub
├── last_capture_region.h/.cpp  # In-memory "last capture region" for the restore-region hotkey
├── save.h/.cpp                 # SaveService — file save service (auto naming, high-quality PDF output)
├── export.h/.cpp               # ExportService — image export
├── clipboard_utils.h/.cpp      # copy_image_to_clipboard() — copy images to system clipboard
├── platform_utils.h/.cpp       # DPI awareness, AppUserModelID, Windows API utilities
├── qt_utils.h/.cpp             # safe_disconnect() — Qt signal safe disconnect
├── constants.h/.cpp            # Global constants (fonts, paths, etc.)
├── background_tasks.h/.cpp     # Background save tracking
├── update_cache.h/.cpp         # Updater cache cleanup
├── updater_process.h/.cpp      # Rust updater JSONL process adapter
├── update_controller.h/.cpp    # Update download and safe restart coordination
├── update_checker.h/.cpp       # Asynchronous GitHub release lookup and version comparison
├── ui_theme.h/.cpp             # UIThemeManager — light/dark appearance for app windows and native Qt widgets
├── east_asian_width.h/.cpp     # is_east_asian_wide — whether a code point is wide or fullwidth (East Asian Width W/F)
├── east_asian_width.inc        # East Asian Width table (generated by a script)
├── hooks.h                     # HookList — callbacks on an object's extension point, called in registration order and removable by id
├── html_entities.inc           # HTML named character reference table (generated by a script)
├── html_unescape.h/.cpp        # html_unescape — decode named and numeric HTML character references
├── http_client.h/.cpp          # http_request — one blocking exchange through the system's WinHTTP with the system proxy; tells HTTP errors, timeouts and network failures apart; cancellable
├── native.h                    # Declarations of the C interface to the Rust libraries (cpp/native)
├── text_utils.h/.cpp           # left_chars — truncate to a number of characters without splitting a surrogate pair
└── serial_executor.h           # SerialExecutor — runs tasks one at a time in submission order on a dedicated thread
```

</details>

**Core Features:**
- Unified logging to files and the console
- Global crash handling that captures unhandled exceptions
- Unified loading of SVG and image resources
- Application-wide light/dark theme management
- XML-based i18n system (Chinese / English / Japanese / Korean / Traditional Chinese)
- Global hotkey system (Windows API) and in-app shortcut management; with "Pause Hotkeys While Gaming" on in the tray menu, global hotkeys pause while a game is in front
- File save/export services (including high-quality PDF output)
- Run as administrator: a scheduled task starts it at sign-in without a UAC prompt, and a normal launch hands over to the elevated instance
- System HTTP requests (WinHTTP, using the system proxy) and the declarations of the Rust libraries' C interface

---

### gif/ — GIF Recording Module

Screen recording, editing, playback, and export to GIF/video.

Choose **MP4** on the recording toolbar for direct H.264 recording, with custom 1–60 fps, 0.5–50 Mbps target bitrate, optional system audio and mouse pointer. Hardware encoding is preferred when available, with Windows software encoding as fallback. Recording runs in a separate worker process (the same exe started with `--video-recorder-worker`) that streams frames to the video without keeping them in the GUI process; stopping waits for MP4 finalization before reporting success. MP4 recordings are saved directly and do not enter the GIF editing/playback workflow. Video is output as SDR (on HDR displays frames are captured through DXGI and mapped to SDR, so there is no HDR video), and system audio comes from the default playback device (up to 8 channels, down-mixed to stereo) and excludes the microphone.

> [!NOTE]
> Windows Media Foundation/H.264/AAC components must be available; Windows N editions may need the Media Feature Pack. No FFmpeg or additional codec bundle is included.

<details>
<summary>Expand directory structure</summary>

```text
gif/
├── record_window.h/.cpp        # GifRecordWindow / AppState — state machine coordinator (3-layer window)
├── overlay.h/.cpp              # CaptureOverlay / OverlayMode — capture overlay, region adjustment
├── drawing_view.h/.cpp         # GifDrawingView / GifDrawingScene — drawing during recording
├── record_toolbar.h/.cpp       # RecordToolbar — start/pause/stop controls
├── frame_recorder.h/.cpp       # FrameRecorder / FrameData / CursorSnapshot — frame sampling
├── video_recorder.h/.cpp       # VideoRecorder — bounded JSONL control of the MP4 recording worker
├── video_settings.h/.cpp       # RecordingOptions — independent video FPS/bitrate/audio settings
├── playback_engine.h/.cpp      # PlaybackEngine / PlayState — frame playback and preview
├── playback_controller.h/.cpp  # PlaybackController — playback UI and export management
├── playback_toolbar.h/.cpp     # PlaybackToolbar / RangeSlider — progress bar, speed control
├── composer.h/.cpp             # ComposeWorker / ComposerProgressDialog — GIF/video composition
├── cursor_overlay.h/.cpp       # CursorOverlay — cursor rendering and click animation
├── _widgets.h/.cpp             # ClickMenuButton / svg_icon() — custom widgets
└── gifrecorder.h/.cpp          # C++ handles over the native gifrecorder — frame store, frame decoder, recording session, cursor sprites
```

</details>

**Core Features:**
- State-machine driven recording flow (selection → recording → playback → export)
- Three-layer window architecture: overlay (selection), drawing layer (annotation), toolbar layer
- Frame-sampled recording with cursor capture and click animation
- Playback preview with range trimming and speed control
- Export to GIF or video (through the Rust library gifrecorder)

---

### ocr/ — OCR Module

Text recognition management with two engines: the OCR built into the Windows Snipping Tool, and PP-OCR.


<details>
<summary>Expand directory structure</summary>

```text
ocr/
├── ocr_manager.h/.cpp        # OCRManager — engine choice, loading, release, unified recognition interface
├── ocr_line_geometry.h/.cpp  # Reading frame and character positions of a recognized line, estimated from character widths when the engine gives none
├── ppocr_models.h/.cpp       # PP-OCR models: models/ folder, built-in catalog, import, removal, download via the updater
├── snipping_tool_ocr.h/.cpp  # SnippingToolOcr — locate the Snipping Tool, copy its OCR files, pad and recognize
└── bundled_runtime.h/.cpp    # bundled_runtime — the onnxruntime.dll embedded in the Full exe, written into models/ at startup when missing or unusable
```

</details>

- Detects engine and model availability automatically
- Settings offer Auto / Windows Snipping Tool / PP-OCR; Auto uses the Snipping Tool when it is installed, otherwise PP-OCR
- Snipping Tool OCR: calls the recognizer that ships with the Windows 11 Snipping Tool directly; faster and reads more languages (including Korean, Russian, and Thai); needs the Windows 11 Snipping Tool
- PP-OCR: the ppocr_rust engine (pure Rust + ONNX Runtime, PP-OCR det + rec); works on any Windows; the Full package carries the runtime and models, the Lite package downloads them in Settings
- PP-OCR models: the Full package includes PP-OCRv6 small; Manage PP-OCR Models in the settings downloads others from a built-in catalog (PP-OCRv6 tiny/medium, PP-OCRv5, and Korean, Latin, Cyrillic, Thai, and Greek recognition) with fixed SHA-256 hashes, or adds local ONNX models after a test recognition. Models live in `models/` next to the program; a selected model that is missing is reported, not replaced by another
- Recognition and release are mutually exclusive; switching engines releases the old one, and the new one loads on the next recognition
- Singleton pattern, unified recognition interface

---

### pin/ — Pin Module

Pin screenshots on screen with editing, zoom, OCR, and translation.


<details>
<summary>Expand directory structure</summary>

```text
pin/
├── pin_window.h/.cpp             # PinWindow — draggable, zoomable, always-on-top image window
├── pin_canvas_view.h/.cpp        # PinCanvasView — pin canvas view (sole content renderer)
├── pin_canvas.h/.cpp             # Pin canvas object
├── pin_manager.h/.cpp            # PinManager — manages all pin windows (singleton)
├── pin_toolbar.h/.cpp            # PinToolbar — pin toolbar
├── pin_controls.h/.cpp           # PinControlButtons — close, edit, copy buttons
├── pin_hover.h/.cpp              # PinHoverControls — decides when hover buttons and toolbar show
├── pin_context_menu.h/.cpp       # PinContextMenu — right-click menu
├── pin_border_overlay.h/.cpp     # PinBorderOverlay — border effect overlay
├── pin_click_through_bar.h/.cpp  # PinClickThroughBar — move / opacity / exit bar while clicks pass through
├── pin_ocr_manager.h/.cpp        # PinOCRManager / PinOcrThread — async OCR recognition
├── pin_shortcut.h/.cpp           # PinShortcutController — normal/edit mode shortcuts
├── pin_thumbnail.h/.cpp          # PinThumbnailMode — thumbnail mode
├── pin_translation.h/.cpp        # PinTranslationHelper — translation helper
├── pin_image_transform.h/.cpp    # PinImageTransform — rotate, flip, etc.
├── ocr_text_layout.h/.cpp        # OcrTextLayout — the pin's recognized text and selection, in image pixels
├── pin_text_selection.h/.cpp     # PinTextSelection — selecting text on a pin by dragging, double-clicking or Ctrl+A, and its highlight
└── pin_text_mode.h/.cpp          # PinTextPanel / PinTextEdit — text mode: the pin shown as editable recognized text
```

</details>

**Core Features:**
- Screenshots stay on top of the screen, draggable and zoomable with the mouse wheel
- Draw and edit directly on a pin
- Built-in OCR: select and copy text right on the pin, still selectable after zooming or rotating
- Text mode: the pin turns into editable recognized text; switch back to the image and again, and the edits and their undo history are still there
- Built-in translation of the text in a pin
- Shortcuts for both normal and edit modes

---

### settings/ — Settings Module

Unified configuration management.

<details>
<summary>Expand directory structure</summary>

```text
settings/
├── app_settings.inc          # The settings table: one row per setting (key, type, default, scope), generating the typed accessors, defaults and import list
├── tool_settings.h/.cpp      # ToolSettingsManager / ToolSettings — tool color, size, hotkey config
├── data_folder.h/.cpp        # At startup: takes in the settings older versions kept in the registry, and keeps the autostart entries in line with the switch
├── settings_transfer.h/.cpp  # Import settings from another settings.ini or a JSON file exported by earlier versions
├── color_formats.h/.cpp      # magnifier color format templates: render, load, save
└── special_paste.h/.cpp      # special paste menu: built-in options, order and on/off state
```

</details>

**Core Features:**
- Singleton configuration manager, with every setting listed in one table
- Manages the colour, line width, font size and other parameters of each drawing tool
- Settings are stored in `settings.ini` (next to the exe or in `%APPDATA%\Jietuba`); those that only make sense on this computer stay in the registry
- On first start the settings older versions kept in the registry are copied into `settings.ini`; nothing is deleted, so going back to an older version still works

---

### stitch/ — Long Screenshot Stitching Module

<details>
<summary>Expand directory structure</summary>

```text
stitch/
├── auto_scroll.h/.cpp                  # AutoScroller — auto scroll: step size from stitch results, stops at the end or on mouse move
├── change_watch.h/.cpp                 # ChangeWatch — watches the capture area in the background and captures whenever the view changes
├── incremental.h/.cpp                  # IncrementalStitcher — background stitching, preview thumbnails
├── jietuba_long_stitch_unified.h/.cpp  # Stitching interface (calls the Rust longstitch)
├── scroll_window.h/.cpp                # ScrollCaptureWindow — scroll capture window
└── scroll_toolbar.h/.cpp               # Scroll capture toolbar
```

</details>

**Core Features:**
- Scrolls the page and captures it, horizontally or vertically
- Scrolling back only repositions without shortening the result; going past the start continues at the other end, so a capture started mid-page can grow both ways
- Auto-scroll: park the cursor in the middle of the capture area and it scrolls by itself, stopping at the end or as soon as the mouse moves
- Trim: scroll back to where you want to cut and remove everything above or below the current view with one click
- Rows that differ by only a few characters, such as file lists and tables, still line up; if one jump overshoots and cannot be joined, it asks you to scroll back instead of forcing a bad match
- Smart stitching based on image matching (finds the overlap)
- A unified long-screenshot interface (accelerated by the Rust library longstitch)

---

### text_recognition/ — Text Recognition Module

Recognizes the text in a screenshot selection and only shows it, without translating: recognition runs on a background thread, the result window first shows “Recognizing…”, then the text appears in an editable box with a copy button.

<details>
<summary>Expand directory structure</summary>

```text
text_recognition/
├── layout.h/.cpp         # layout — lays recognized lines out as text: joins wrapped lines, removes spaces between CJK characters, converts punctuation width
├── placement.h/.cpp      # placement — where the result window goes (positions only, no window access)
├── recognizer.h/.cpp     # RecognizeThread — recognizes one image on a worker thread and reports the result
├── result_window.h/.cpp  # TextRecognitionWindow — the result window: an editable text box, a copy button and the layout menu
└── text_recognition.h    # recognize_async / show_text_recognition — entry points: background recognition, opening the result window, copying the result
```

</details>

- Background recognition that never blocks the UI; recognition is one uninterruptible native call, so cancelling only stops the result from being reported
- Text layout: wrapped lines joined, spaces between CJK characters removed, punctuation width converted; switchable in the window
- The result window sits next to the recognized region and keeps clear of the cursor
- Editable text with one-click copy

---

### tools/ — Drawing Tools Module

Implementations of the drawing tools.

<details>
<summary>Expand directory structure</summary>

```text
tools/
├── base.h/.cpp            # Tool / ToolContext — abstract base class
├── controller.h/.cpp      # ToolController — tool switching and state management
├── action.h/.cpp          # ActionTools — copy, save, cancel actions
├── pen.h/.cpp             # PenTool — freehand drawing
├── rect.h/.cpp            # RectTool — rectangle (filled/outlined)
├── ellipse.h/.cpp         # EllipseTool — ellipse
├── arrow.h/.cpp           # ArrowTool — arrow
├── text.h/.cpp            # TextTool — text
├── number.h/.cpp          # NumberTool — auto-incrementing numbers
├── highlighter.h/.cpp     # HighlighterTool — highlighter
├── mosaic.h/.cpp          # MosaicTool — pixel mosaic
├── spotlight.h/.cpp       # SpotlightTool — spotlight (dims outside the box)
├── watermark.h/.cpp       # WatermarkTool — adds the watermark, changes its text and style, remembers whether the last screenshot had one
├── cursor.h               # CursorTool — cursor/selection
├── eraser.h/.cpp          # EraserTool — eraser
└── cursor_manager.h/.cpp  # CursorManager — cursor style manager
```

</details>

**Core Features:**
- Unified tool base-class architecture (Tool → concrete tools)
- Drawing tools: pen, rectangle, ellipse, arrow, text, number, highlighter, mosaic, watermark, cursor, eraser and more
- The tool controller switches tools and dispatches mouse events
- The tool context (ToolContext) injects the scene, view, settings and other dependencies

---

### translation/ — Translation Module

Multi-provider translation service supporting DeepL, Google, Azure, Amazon, Baidu, Volcengine, Tencent Cloud, Youdao, DeepSeek, and any OpenAI-compatible endpoint you configure.

<details>
<summary>Expand directory structure</summary>

```text
translation/
├── provider.h/.cpp                      # TranslationProvider / ProviderMetadata — provider contract
├── registry.h/.cpp                      # ProviderRegistry — provider registration and factory
├── service.h/.cpp                       # TranslationService — provider selection and orchestration
├── models.h/.cpp                        # TranslationRequest / TranslationResult — provider-neutral models
├── worker.h/.cpp                        # TranslationWorker — shared Qt worker for all providers
├── providers/                           # provider implementations
│   ├── deepl.h/.cpp                     # DeepL
│   ├── google.h/.cpp                    # Google
│   ├── azure.h/.cpp                     # Azure
│   ├── amazon.h/.cpp                    # Amazon
│   ├── baidu.h/.cpp                     # Baidu
│   ├── tencent.h/.cpp                   # Tencent Cloud
│   ├── volcengine.h/.cpp                # Volcengine
│   ├── youdao.h/.cpp                    # Youdao
│   ├── custom_llm.h/.cpp                # Custom OpenAI-compatible service (Ollama, LM Studio…)
│   ├── deepseek.h/.cpp                  # DeepSeek (LLM)
│   ├── openai_compatible.h/.cpp         # Base class for OpenAI-compatible endpoints
│   └── common.h                         # Response parsing and error-body helpers shared by the providers
├── smart_translation_controller.h/.cpp  # SmartTranslationController — one-hotkey text probe and popup routing
├── translation_popup.h/.cpp             # TranslationPopup — compact popup (selected text / typed input)
├── deepl_service.h/.cpp                 # DeepLService / DeepLResult — legacy async DeepL API calls
├── languages.h                          # translation_languages / source_languages — supported language list & codes
├── translation_manager.h/.cpp           # TranslationManager — translation window manager (singleton)
├── translation_dialog.h/.cpp            # TranslationDialog — translation result window
├── speech_service.h/.cpp                # SpeechService / SapiBackend — reads translations aloud through the system SAPI, matching voices by language, one reading at a time
└── ui/
    └── widgets.h/.cpp                   # Translation widgets
```

</details>

**Core Features:**
- Pluggable multi-provider architecture with a unified registry
- One hotkey: probes selected text and routes it to the popup
- Compact popup with selected-text and typed-input modes
- Async translation that never blocks the UI
- Copyable results
- Read translations aloud with the system voices, matched by language, one reading at a time

---

### translations/ — Language Resources

The interface texts in several languages, in `cpp/resources/translations/`.

<details>
<summary>Expand directory structure</summary>

```text
translations/
├── app_zh.xml / app_en.xml  # Chinese / English source files
├── app_ja.xml / app_ko.xml  # Japanese / Korean source files
├── app_zh_TW.xml            # Traditional Chinese (Taiwan) source file
└── app_*.qm                 # compiled Qt binaries (e.g. app_zh.qm)
```

</details>

`.xml` = editable source files, `.qm` = compiled Qt runtime files. When adding or changing UI text, add an entry with the same context to all five `.xml` files (use real line breaks in multi-line text), then run `scripts/compile_translations.py`. Log texts are edited directly in `resources/core/log_translations.json`.

---

### ui/ — UI Module

Common UI component library.

<details>
<summary>Expand directory structure</summary>

```text
ui/
├── toolbar.h/.cpp                   # Toolbar / ToolbarDragHandle — draggable toolbar base class
├── toolbar_layout.h/.cpp            # screenshot / pin toolbar button layouts (order / visibility): normalize, load, save
├── toolbar_layout_dialog.h/.cpp     # ToolbarLayoutDialog — screenshot / pin toolbar layout editor
├── reorderable_rows.h/.cpp          # DragGrip / DraggableRow / ReorderableRowList — drag-to-reorder rows
├── tray_menu.h/.cpp                 # CustomTrayMenu — system tray menu
├── screenshot_window.h/.cpp         # ScreenshotWindow — full-screen capture window (region drawing)
├── quick_capture_overlay.h/.cpp     # Transparent quick capture layer reusing the normal selection, coordinates and magnifier
├── update_dialog.h/.cpp             # Update notes and download progress
├── dialogs.h/.cpp                   # StandardDialog — confirm, warning, info, error dialogs
├── toast.h/.cpp                     # Toast — one-line hint by the cursor that never takes focus
├── submenu_position.h/.cpp          # Cascading submenus: which way they open and how they hug the parent item
├── magnifier.h/.cpp                 # MagnifierOverlay — pixel-level magnifier
├── color_picker_button.h/.cpp       # ColorPickerButton — color selection button
├── color_picker_dialog.h/.cpp       # Color picker window: saturation/value square, hue and opacity sliders, eyedropper
├── save_image_dialog.h/.cpp         # SaveImageDialog — save image dialog: frequent folder tags, format and quality
├── screenshot_countdown.h/.cpp      # ScreenshotCountdown — countdown before a delayed screenshot
├── hotkey_edit.h/.cpp               # HotkeyEdit — global hotkey editor
├── inapp_key_edit.h/.cpp            # InAppKeyEdit — in-app shortcut editor
├── key_chip.h/.cpp                  # KeyChipLineEdit / StatusIcon — shared shortcut chip look
├── mask_overlay.h/.cpp              # mask overlay layer
├── selection_overlay.h/.cpp         # SelectionOverlayWidget — selection chrome layer above the mask
├── base_settings_panel.h/.cpp       # BaseSettingsPanel / StepperWidget — settings panel base class
├── paint_settings_panel.h/.cpp      # PaintSettingsPanel — brush settings panel
├── shape_settings_panel.h/.cpp      # ShapeSettingsPanel — shape settings panel
├── text_settings_panel.h/.cpp       # TextSettingsPanel — text settings panel
├── arrow_settings_panel.h/.cpp      # ArrowSettingsPanel — arrow settings panel
├── number_settings_panel.h/.cpp     # number tool settings panel
├── mosaic_settings_panel.h/.cpp     # mosaic tool settings panel
├── watermark_settings_panel.h/.cpp  # watermark panel: text, position, style popup (opacity/size/spacing/angle/grid lines/color), font
├── watermark_input_dialog.h/.cpp    # WatermarkInputDialog — multi-line watermark text with time insertion and preview
├── initial_selection.h/.cpp         # InitialSelectionGate — holds the capture window back until the first point query returns (at most 50 ms), so the first frame already shows a selection
│
├── fluent_lite/                     # Fluent-style lightweight component library
│   ├── buttons.h/.cpp               # buttons
│   ├── cards.h/.cpp                 # cards
│   ├── icons.h/.cpp                 # icons
│   ├── inputs.h/.cpp                # inputs
│   ├── labels.h/.cpp                # labels
│   ├── navigation.h/.cpp            # navigation
│   ├── segmented.h/.cpp             # segmented controls
│   ├── switch.h/.cpp                # switch
│   ├── theme.h/.cpp                 # theme
│   ├── titlebar.h/.cpp              # title bar
│   ├── frameless.h/.cpp             # frameless windows
│   ├── text_context_menu.h/.cpp     # text context menu
│   └── fluent_lite.h/.cpp           # Umbrella header and the check button
│
├── settings_ui/                     # Application settings dialog
│   ├── dialog.h/.cpp                # SettingsDialog — tabbed settings dialog
│   ├── components.h/.cpp            # SettingCardGroup / ToggleSwitch — setting components
│   ├── page_appearance.cpp          # Appearance settings (theme, language)
│   ├── page_capture.cpp             # Capture settings
│   ├── page_quick_actions.cpp       # Quick actions settings (skip confirm or result windows)
│   ├── color_format_dialog.h/.cpp   # ColorFormatDialog — magnifier color format editor
│   ├── ppocr_model_dialog.h/.cpp    # PpocrModelDialog — download, add, and delete PP-OCR models
│   ├── page_clipboard.cpp           # Clipboard settings
│   ├── special_paste_dialog.h/.cpp  # SpecialPasteDialog — reorder and toggle special paste menu items
│   ├── page_hotkey.cpp              # Hotkey settings
│   ├── page_mouse.cpp               # Mouse shortcut settings
│   ├── page_pin.cpp                 # Pin settings (behavior, appearance, click-through)
│   ├── page_screen_annotation.cpp   # Screen annotation settings
│   ├── page_translation.cpp         # Translation settings
│   ├── provider_fields.cpp          # Translation provider fields: read/write config and widget values from declarations
│   ├── page_log.cpp                 # Log settings
│   ├── page_developer.cpp           # Developer settings
│   ├── page_misc.cpp                # Miscellaneous settings
│   ├── page_about.cpp               # About page
│   ├── mouse_binding_editor.h       # MouseBindingEditor — picker for a mouse shortcut's modifiers and gesture
│   └── pages.h                      # Declarations of the settings page builders
│
├── welcome/                         # First-run welcome wizard (8-page guided setup)
│   ├── wizard.h/.cpp                # WelcomeWizard — wizard main window
│   ├── base_page.h/.cpp             # BasePage — wizard page base class
│   ├── page1_welcome.cpp            # Welcome page
│   ├── page2_screenshot.cpp         # Screenshot hotkey setup page
│   ├── page3_clipboard.cpp          # Clipboard hotkey setup page
│   ├── page5_translation.cpp        # Translation feature intro page
│   ├── page6_finish.cpp             # Finish page
│   ├── page_hotkeys.cpp             # Global hotkey page — six hotkeys share one conflict domain, set and validated together
│   ├── page_mouse_shortcuts.cpp     # Global mouse shortcut page — hold modifiers and drag to capture or act at once
│   ├── page_screen_annotation.cpp   # Screen annotation page — a demo and its hotkeys
│   └── pages.h                      # Declarations of the wizard pages
│
└── selection_info/                  # Selection info UI
    ├── controller.h/.cpp            # Selection info controller
    ├── panel.h/.cpp                 # Selection info panel (dimensions, coordinates)
    ├── border_shadow.h/.cpp         # Selection border shadow effect
    ├── lock_ratio.h/.cpp            # Aspect ratio lock
    └── rounded_corners.h/.cpp       # Rounded corner capture
```

</details>

**Core Features:**
- Draggable toolbar base class that every toolbar inherits from
- Full-screen screenshot window that handles selection drawing and interaction
- Fine-grained UI components such as the magnifier, the color picker window and the save image dialog
- Lightweight Fluent-style component library (buttons, cards, navigation, segmented controls, switches and more)
- Complete application settings dialog (appearance / capture / hotkeys / translation / screen annotation and other pages)
- First-run welcome wizard (an eight-page guided flow)
- Selection info panel (size readout, aspect-ratio lock, rounded corners and more)

---

### video/ — MP4 Recording Worker

The MP4 recording worker process: the same exe started with `--video-recorder-worker`. The native recorder runs only in this process, so frames never enter the GUI process. Commands arrive as JSON lines on standard input and events leave as JSON lines on standard output.

<details>
<summary>Expand directory structure</summary>

```text
video/
└── worker.h/.cpp  # worker_main — entry of the MP4 recording process: JSON-line commands on stdin, JSON-line events on stdout; frames stay inside the native recorder
```

</details>

- Records H.264 video at 1–60 fps with a 0.5–50 Mbps target bitrate
- Optional system audio (the default playback device) and mouse pointer; video is SDR
- Success is reported only after the MP4 has been finalized

---

### tests/ — Test Module

QtTest tests. Tests whose names end in `_desktop` run on the real desktop, the rest can run offscreen; every test program links `test_sandbox.cpp`, so settings, data and logs land in a temporary folder.

<details>
<summary>Expand directory structure</summary>

```text
tests/
├── test_barcode.cpp               # QR code and barcode reading
├── test_bundled_runtime.cpp       # Writing the Full package's embedded runtime
├── test_canvas_items.cpp          # Canvas items: handles, borders and appearance settings
├── test_canvas_parity.cpp         # Canvas drawing geometry, pixels, undo and ownership
├── test_canvas_view.cpp           # Canvas view
├── test_capture_finders.cpp       # Window lookup and the system pointer on the real desktop (desktop test)
├── test_clipboard.cpp             # Clipboard logic and context menus
├── test_clipboard_backend.cpp     # Clipboard backend: the database and the system clipboard watcher
├── test_clipboard_controller.cpp  # Clipboard window controller: paging, filters, groups, pasting
├── test_clipboard_drag.cpp        # Dragging clipboard items out: the formats each target gets and files written on demand
├── test_clipboard_manage.cpp      # Clipboard management window
├── test_clipboard_selection.cpp   # Keyboard and mouse selection in the clipboard list
├── test_clipboard_ui.cpp          # The clipboard backend, windows and settings together
├── test_clipboard_widgets.cpp     # Clipboard widgets: row painting, preview popups, gear menu, themes
├── test_clipboard_window.cpp      # Clicks, keys and menus of the clipboard window
├── test_core.cpp                  # Core: global hotkeys, log texts, the administrator task, single-instance hand-off
├── test_data_folder.cpp           # Settings file location, registry vs INI, migrating older settings, autostart entries
├── test_freehand.cpp              # Pen stroke smoothing, snapping and chunked repainting
├── test_gif.cpp                   # GIF/MP4 recording, playback and export
├── test_gif_playback.cpp          # GIF playback: decoding and playing at the display size
├── test_hdr_capture.cpp           # HDR capture (desktop test)
├── test_http_providers.cpp        # The HTTP client and providers with a configurable address
├── test_input_engine.cpp          # Input state machine: gestures, hotkeys, shortcut recording
├── test_input_hooks.cpp           # The input hook thread on the real desktop, driven by injected input (desktop test)
├── test_logger.cpp                # Logging
├── test_main_app.cpp              # Main app: tray, hotkeys, capture flow and global input (desktop test)
├── test_msaa_element_finder.cpp   # Point query and first-frame selection
├── test_ocr_manager.cpp           # OCR engine and model management
├── test_parity.cpp                # Selection state, last region, toolbar layout, scale rounding, setting defaults and persistence
├── test_pin.cpp                   # Pin: rotation, OCR coordinate mapping and window behaviour
├── test_platform.cpp              # Platform helpers: processes, DPI, autostart, crash log
├── test_ppocr_models.cpp          # PP-OCR model download, adding and catalog
├── test_result_windows.cpp        # OCR and barcode result windows
├── test_sandbox.cpp               # Linked into every test program: points settings, data and logs at a temporary folder
├── test_save_image_dialog.cpp     # Save image dialog: folder tags, format and quality
├── test_screen_annotation.cpp     # Screen annotation: per-screen layers, click-through and focus, cross-screen undo, screens plugged in or out
├── test_screenshot_window.cpp     # Screenshot window
├── test_scroll_capture.cpp        # Scroll capture
├── test_settings_window.cpp       # Settings window
├── test_shortcut_manager.cpp      # Shortcut manager and global hooks
├── test_smart_selection.cpp       # Smart selection over real windows (desktop test)
├── test_snipping_tool_ocr.cpp     # Snipping Tool OCR binding (against a fake oneocr.dll)
├── test_stitch.cpp                # Long screenshot stitching
├── test_text_recognition.cpp      # Text recognition: layout and window placement
├── test_translation.cpp           # Translation providers, registry and service
├── test_translation_speech.cpp    # Reading translations aloud (a fake backend, no system voice)
├── test_translation_ui.cpp        # Translation window and popup
├── test_ui_widgets.cpp            # Shared widgets: toolbar, settings panels, hotkey editors and more
├── test_update.cpp                # Update check and one-click update (against a fake updater)
├── clipboard_test_support.h       # Shared setup of the clipboard tests
├── caption_check.h                # Checks that self-drawn windows share one caption layout (icon on the left, then the title)
├── save_dialog_answerer.h         # Answers the save image dialog in tests
├── fake_oneocr.cpp                # Stand-in for the Snipping Tool's oneocr.dll in tests (including variants missing functions)
├── fake_updater.cpp               # Stand-in for the updater helper in tests
├── leak_check.h/.cpp              # Leak mode: runs the tests three times and finds leaks on the debug heap
└── fixtures/                      # Expected-result data (committed, not regenerated)
```

</details>

---

## License

Copyright and the MIT license are in [LICENSE](LICENSE); Qt and other third-party components are described in [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md).
