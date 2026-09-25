[中文](README_zh-CN.md) | **[English](README.md)** | [日本語](README_JA.md)

# jietuba — Screenshot, OCR, Pin, Translation & Clipboard Tool for Windows

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/ci.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Download for Windows](https://github.com/1003129155/jietuba/releases/latest) · [Run from Source](#source-setup) · [Development and Tests](#development)

![jietuba demo](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

## Overview

jietuba is a free, open-source screenshot tool for Windows: region and window capture, scrolling (long) screenshots, annotation, OCR text recognition, translation, image pinning, GIF recording, QR code and barcode scanning, PDF export, and a full clipboard history manager. Everything runs locally.

The interface is built with PySide6; image processing, clipboard access, and OCR are implemented in Rust. Runs on Windows x86_64 and ARM64.

Download a ready-to-run Windows release or run the application from source.

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

The Windows x86_64 and ARM64 releases are ready to run. You do not need to install Python, Rust, or a development environment.

1. Open the [Releases page](https://github.com/1003129155/jietuba/releases/latest) and download the archive ending in `-x64.zip` or `-arm64.zip` for your device.
2. Extract the entire archive, keeping `jietuba_pp.exe` and the `models/` folder together.
3. Double-click `jietuba_pp.exe` to start. The OCR models are included in the archive.
4. The application is not digitally signed, so Windows may display a warning after you download it through a browser. If prompted, click **More info**, then **Run anyway** to start the application.

---

<a id="source-setup"></a>

## Run from Source

All runtime dependencies install through [requirements.txt](requirements.txt).

### One-Click Setup

1. Install Python 3.11 for your Windows architecture (x64 or ARM64), including the Python Launcher.
2. Download and extract or clone this repository, then double-click [setup.bat](setup.bat) in the project root.

### Manual Setup

Install Python 3.11 for your Windows architecture (x64 or ARM64), including the Python Launcher, then open Windows Command Prompt (CMD) in the project root and run these steps:

**1. Create and activate a virtual environment**

```bat
py -3.11 -m venv venv311
call venv311\Scripts\activate.bat
```

**2. Install all runtime dependencies**

```bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

**3. Run the application**

```bat
cd main
python main_app.py
```

The repository includes `PP-OCRv6_det_small.onnx` and `PP-OCRv6_rec_small.onnx` in [models/](models/). Keep that directory in place for OCR.

### Rust Extension Packages

These four packages are included in `requirements.txt` and install with the runtime dependencies. They can also be used independently; their source code is in [rust_libs/](rust_libs/). Their PyPI distribution names map to Python import names as follows:

| pip name | import name | Version | Description |
|------|------|------|------|
| [`j-gif`](https://pypi.org/project/j-gif/) | `gifrecorder` | 0.3.1 | GIF/video composition encoder |
| [`j-stitch`](https://pypi.org/project/j-stitch/) | `longstitch` | 0.4.0 | Long screenshot stitching algorithm |
| [`j-clipboard`](https://pypi.org/project/j-clipboard/) | `pyclipboard` | 0.4.2 | Low-level clipboard operations |
| [`j-ppocr`](https://pypi.org/project/j-ppocr/) | `ppocr_rust` | 0.2.0 | PP-OCR (PaddleOCR) ONNX text recognition (pure Rust + ONNX Runtime, needs det/rec models) |

The available prebuilt wheels target Windows x86_64 and ARM64. Each package declares `>=3.11` and enables `abi3-py311` in its Rust bindings; see each package's `pyproject.toml` and `Cargo.toml`.

---

<a id="development"></a>

## Development and Tests

From the project root, with the virtual environment activated, install development dependencies and run the tests:

```bat
python -m pip install -r requirements-dev.txt
python -m pytest main/tests -c main/tests/pytest.ini
```

The [test directory](main/tests/) contains unit and integration tests for capture, clipboard operations, mosaic editing, pin zoom, GIF playback, OCR text layers, and other modules. The [CI workflow](.github/workflows/ci.yml) runs tests and coverage checks on Windows x86_64 and ARM64 with Python 3.11, plus static analysis on x86_64. Results are available in [GitHub Actions](https://github.com/1003129155/jietuba/actions/workflows/ci.yml).

To build a Windows release, run `python build_with_ocr_onefile.py`. It produces `dist/jietuba_pp.exe` and `dist/models/`. The automated [release workflow](.github/workflows/build.yml) creates separate x64 and ARM64 archives.

---

## Directory Structure

<details>
<summary>Expand directory structure</summary>

```text
# Project root
├── README.md / README_zh-CN.md / README_JA.md          # English, Chinese, and Japanese documentation
├── pyproject.toml                                      # Python project metadata and dependency declarations
├── requirements.txt                                   # Runtime dependencies
├── requirements-dev.txt                               # Test and build dependencies
├── build_with_ocr_onefile.py                           # PyInstaller one-file build script
│
├── main/                    # Python main program
│   ├── main_app.py          # App entry point: system tray, global hotkeys, lifecycle management
│   ├── compile_translations.py  # Translation compiler (.xml → .qm)
│   ├── scripts/             # Helper scripts — translation provider comparison
│   │
│   ├── barcode/             # Barcode module — QR code/barcode scanning (zxing-cpp)
│   ├── canvas/              # Canvas module — graphics editing core
│   ├── capture/             # Capture module — screen capture & window detection
│   ├── clipboard/           # Clipboard module — history, groups/quick launch, import/export, search
│   ├── core/                # Core module — bootstrap, logging, resources, theme, i18n, hotkeys
│   ├── gif/                 # GIF module — screen recording, editing, playback, export
│   ├── ocr/                 # OCR module — PP-OCR text recognition
│   ├── pin/                 # Pin module — pinned screenshots, editing, OCR, translation
│   ├── settings/            # Settings module — unified configuration management
│   ├── stitch/              # Stitch module — scroll capture, auto-stitching
│   ├── tools/               # Tools module — pen, rect, arrow, text, mosaic, etc.
│   ├── translation/         # Translation module — multi-provider translation service
│   ├── translations/        # Language resources — Chinese/English/Japanese/Korean
│   ├── ui/                  # UI module — common UI component library
│   └── tests/               # Tests module — unit tests & integration tests
│
├── rust_libs/               # Rust library source code (buildable from source)
│   ├── gifrecorder/         # GIF/video composition encoder source
│   ├── longstitch/          # Long screenshot stitching algorithm source
│   ├── pyclipboard/         # Low-level clipboard operations source
│   └── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX recognition engine source
│
├── models/                  # PP-OCR ONNX models (required for OCR)
│   ├── PP-OCRv6_det_small.onnx   # text detection model (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # text recognition model (CRNN/CTC)
│
└── svg/                     # SVG icon assets
```

</details>

---

## Module Details

### barcode/ — Barcode Module

Scans QR codes and barcodes in the screenshot selection. The result window outlines each code on the screenshot and lists the decoded contents by number.

<details>
<summary>Expand directory structure</summary>

```text
barcode/
├── __init__.py
├── reader.py                # read_codes / DecodedCode — zxing-cpp decoding; text, format and outline in reading order
└── result_window.py         # BarcodeResultWindow — result window; screenshot and result list share the same numbers
```

</details>

- Supports QR Code, Data Matrix, Aztec, PDF417 and common linear barcodes such as Code 128 and EAN/UPC
- Reads every code in the selection at once; hovering a code on either side highlights it on both
- One-click copy; http(s) links open directly in the browser

---

### canvas/ — Canvas Module

Graphics editing canvas system with scene management, view rendering, item selection, and undo/redo.
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

<details>
<summary>Expand directory structure</summary>

```text
canvas/
├── __init__.py
├── scene.py                 # CanvasScene — canvas scene, extends QGraphicsScene
├── view.py                  # CanvasView — canvas view, extends QGraphicsView
├── selection_model.py       # SelectionModel — manages selected graphics items
├── undo.py                  # CommandUndoStack — undo/redo stack (add, delete, batch, edit commands)
├── smart_edit_controller.py # SmartEditController — handles selection/edit mode switching
├── smart_selection_anim.py  # SmartSelectionAnimator — tweens the selection when it hops between windows
├── handle_editor.py         # LayerEditor / EditHandle — control point drag editing
├── gestures.py              # Mouse gesture state machines — text edge drag, rubber-band select, pending click-to-edit
├── handle_overlay.py        # HandleOverlay — separate compositing layer for edit handles, avoids full-scene repaint
└── items/
    ├── drawing_items.py     # StrokeItem / RectItem / EllipseItem / NumberItem — items sharing DrawingItemMixin
    ├── background_item.py   # BackgroundItem — selection area background
    ├── mosaic_item.py       # MosaicItem — pixel mosaic item
    ├── spotlight_item.py    # SpotlightItem / SpotlightCurtain — spotlight holes and their shared curtain
    ├── selection_item.py    # SelectionItem — selection boundary display
    ├── arrow_item.py        # ArrowItem — arrow item, geometry for nine shaft and head styles
    └── text_item.py         # TextItem — text item, outline/shadow/background and tri-state interaction frame
```

</details>

---

### capture/ — Capture Module

Screen capture and smart window detection.

<details>
<summary>Expand directory structure</summary>

```text
capture/
├── capture_service.py       # CaptureService — core screenshot logic
├── uia_element_finder.py    # Background UI Automation element snapshots for smart selection
└── window_finder.py         # WindowFinder — smart window selection, cursor-based detection
```

</details>

---

### clipboard/ — Clipboard Management Module

Ditto-like clipboard history manager, now organized into controllers, core, services, and ui layers.
Supports text, images, HTML, files, a dedicated three-pane management window, and pin creation from history items.
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

<details>
<summary>Expand directory structure</summary>

```text
clipboard/
├── __init__.py
├── controllers/             # Control layer — history loading, paste flow, menus, selection state
│   ├── clipboard_controller.py   # ClipboardController — loading, pasting, context menu logic
│   ├── selection_manager.py      # SelectionManager — list selection state
│   ├── context_menu_controller.py  # ContextMenuController — assembles context menu data and actions
│   ├── foreground_tracker.py    # ForegroundWindowTracker — remembers the window to paste into
│   ├── paste_keystroke.py       # Restores focus to the target window, then sends Ctrl+V
│   └── __init__.py
├── core/                    # Data layer — pyclipboard wrapper, models, group types
│   ├── manager.py           # ClipboardManager — storage, monitoring, and paste API
│   ├── models.py            # ClipboardItem / Group models
│   ├── enums.py             # GroupType definitions
│   ├── text_transform.py    # Plain-text transforms — pure functions behind "paste special"
│   └── __init__.py
├── services/                # Service layer — file payloads, group rules, import/export, save logic
│   ├── file_payload_service.py   # file payload JSON + legacy format compatibility
│   ├── group_service.py          # group icon/name/delete helpers
│   ├── import_export_service.py  # CSV import/export for text items
│   └── manage_dialog_service.py  # persistence logic for the management window
├── ui/
│   ├── layout_scale.py           # Shared size metrics for the management dialog
│   ├── image_item_actions.py     # load and save-as for image items, shared by both windows
│   ├── dialogs/
│   │   └── manage_dialog.py      # three-pane management window for groups and content
│   ├── forms/
│   │   ├── group_form.py
│   │   ├── text_content_form.py
│   │   ├── file_content_form.py
│   │   ├── import_export_form.py
│   │   ├── group_icon_picker.py
│   │   ├── form_widgets.py
│   │   └── image_content_form.py
│   ├── menus/
│   │   ├── action_menu.py
│   │   ├── group_context_menu.py
│   │   └── item_context_menu.py
│   ├── mixins/
│   │   └── frameless_mixin.py
│   ├── panels/
│   │   └── setting_panel.py
│   ├── resources/
│   │   └── emoji_data.py
│   ├── theme/
│   │   ├── themes.py
│   │   └── theme_styles.py
│   ├── widgets/
│   │   ├── group_bar.py
│   │   ├── item_delegate.py
│   │   ├── item_widget.py
│   │   ├── preview_popup.py
│   │   ├── quick_edit_popup.py
│   │   ├── manage_rows.py
│   │   └── reorder_list.py
│   └── windows/
│       ├── clipboard_window.py   # history window, search, preview, quick paste
│       └── pin_window.py         # create pins from history items
```

</details>

**Core Features:**
- Monitor clipboard changes and store history automatically
- Support text, images, HTML, and files
- Support general groups, quick-launch groups, favorites, and search
- Dedicated three-pane management window for editing groups, text items, and file items
- CSV import/export for text items
- Themeable UI, quick paste shortcuts, and large image/long text preview popups

---

### core/ — Core Module

Logging, resource loading, theme management, i18n, hotkeys, and other infrastructure.

<details>
<summary>Expand directory structure</summary>

```text
core/
├── bootstrap.py             # PreloadManager — startup bootstrap, env init, DPI, single instance
├── logger.py                # Logger — file + console logging (debug/info/warning/error/exception)
├── crash_handler.py         # install_crash_hooks() — global exception catching
├── resource_manager.py      # ResourceManager — SVG/image resource loading
├── theme.py                 # ThemeManager — application theme colors
├── ui_scale.py              # UIScaleManager — one scale factor for toolbars, panels and popups
├── i18n.py                  # I18nManager / XmlTranslator / tr() — internationalization
├── shortcut_manager.py      # HotkeySystem / ShortcutManager — global & in-app hotkeys
├── last_capture_region.py   # In-memory "last capture region" for the restore-region hotkey
├── save.py                  # SaveService — file save service (auto naming, high-quality PDF output)
├── export.py                # ExportService — image export
├── clipboard_utils.py       # copy_image_to_clipboard() — copy images to system clipboard
├── platform_utils.py        # DPI awareness, AppUserModelID, Windows API utilities
├── qt_utils.py              # safe_disconnect() — Qt signal safe disconnect
├── log_translations/        # per-module log text translation helpers
├── constants.py             # Global constants (fonts, paths, etc.)
├── update_checker.py        # Asynchronous GitHub release lookup and version comparison
└── ui_theme.py              # UIThemeManager — light/dark appearance for app windows and native Qt widgets
```

</details>

---

### gif/ — GIF Recording Module

Screen recording, editing, playback, and export to GIF/video.
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
<details>
<summary>Expand directory structure</summary>

```text
gif/
├── record_window.py         # GifRecordWindow / AppState — state machine coordinator (3-layer window)
├── overlay.py               # CaptureOverlay / OverlayMode — capture overlay, region adjustment
├── drawing_view.py          # GifDrawingView / GifDrawingScene — drawing during recording
├── drawing_toolbar.py       # GifDrawingToolbar — drawing tools toolbar
├── record_toolbar.py        # RecordToolbar — start/pause/stop controls
├── frame_recorder.py        # FrameRecorder / FrameData / CursorSnapshot — frame sampling
├── playback_engine.py       # PlaybackEngine / PlayState — frame playback and preview
├── playback_controller.py   # PlaybackController — playback UI and export management
├── playback_toolbar.py      # PlaybackToolbar / RangeSlider — progress bar, speed control
├── composer.py              # _ComposeWorker / ComposerProgressDialog — GIF/video composition
├── cursor_overlay.py        # CursorOverlay — cursor rendering and click animation
└── _widgets.py              # ClickMenuButton / svg_icon() — custom widgets
```

</details>

---

### ocr/ — OCR Module

Text recognition management powered by PP-OCR.

<img width="580" height="505" alt="image" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />

<details>
<summary>Expand directory structure</summary>

```text
ocr/
└── ocr_manager.py           # OCRManager — text recognition via ppocr_rust (PP-OCR)
```

</details>

- Powered by the ppocr_rust engine (pure Rust + ONNX Runtime, PP-OCR det + rec); inference runs on native threads without blocking the UI
- Chinese/English/Japanese recognition
- Singleton pattern, unified recognition interface

---

### pin/ — Pin Module

Pin screenshots on screen with editing, zoom, OCR, and translation.

<img width="737" height="657" alt="image" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />

<details>
<summary>Expand directory structure</summary>

```text
pin/
├── pin_window.py            # PinWindow — draggable, zoomable, always-on-top image window
├── pin_canvas_view.py       # PinCanvasView — pin canvas view (sole content renderer)
├── pin_canvas.py            # Pin canvas object
├── pin_manager.py           # PinManager — manages all pin windows (singleton)
├── pin_toolbar.py           # PinToolbar — pin toolbar
├── pin_controls.py          # PinControlButtons — close, edit, copy buttons
├── pin_context_menu.py      # PinContextMenu — right-click menu
├── pin_border_overlay.py    # PinBorderOverlay — border effect overlay
├── pin_ocr_manager.py       # PinOCRManager / _OCRThread — async OCR recognition
├── pin_shortcut.py          # PinShortcutController — normal/edit mode shortcuts
├── pin_thumbnail.py         # PinThumbnailMode — thumbnail mode
├── pin_translation.py       # PinTranslationHelper — translation helper
├── pin_image_transform.py   # PinImageTransform — rotate, flip, etc.
└── ocr_text_layer.py        # OCRTextLayer / OCRTextItem — OCR text layer display
```

</details>

---

### settings/ — Settings Module

<details>
<summary>Expand directory structure</summary>

```text
settings/
├── color_formats.py         # magnifier color format templates: render, load, save
└── tool_settings.py         # ToolSettingsManager / ToolSettings — tool color, size, hotkey config
```

</details>

---

### stitch/ — Long Screenshot Stitching Module
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)

<details>
<summary>Expand directory structure</summary>

```text
stitch/
├── jietuba_long_stitch_unified.py   # Stitching interface (calls the Rust longstitch)
├── scroll_window.py                 # ScrollCaptureWindow — scroll capture window
└── scroll_toolbar.py                # Scroll capture toolbar
```

</details>

---

### tools/ — Drawing Tools Module

<details>
<summary>Expand directory structure</summary>

```text
tools/
├── base.py                  # Tool / ToolContext — abstract base class
├── controller.py            # ToolController — tool switching and state management
├── action.py                # ActionTools — copy, save, cancel actions
├── pen.py                   # PenTool — freehand drawing
├── rect.py                  # RectTool — rectangle (filled/outlined)
├── ellipse.py               # EllipseTool — ellipse
├── arrow.py                 # ArrowTool — arrow
├── text.py                  # TextTool — text
├── number.py                # NumberTool — auto-incrementing numbers
├── highlighter.py           # HighlighterTool — highlighter
├── mosaic.py                # MosaicTool — pixel mosaic
├── spotlight.py             # SpotlightTool — spotlight (dims outside the box)
├── cursor.py                # CursorTool — cursor/selection
├── eraser.py                # EraserTool — eraser
└── cursor_manager.py        # CursorManager — cursor style manager
```

</details>

---

### translation/ — Translation Module

Multi-provider translation service supporting DeepL / Google / Azure / Amazon.

<details>
<summary>Expand directory structure</summary>

```text
translation/
├── provider.py              # TranslationProvider / ProviderMetadata — provider contract
├── registry.py              # ProviderRegistry — provider registration and factory
├── service.py               # TranslationService — provider selection and orchestration
├── models.py                # TranslationRequest / TranslationResult — provider-neutral models
├── worker.py                # TranslationWorker — shared Qt worker for all providers
├── providers/               # provider implementations
│   ├── deepl.py             # DeepL
│   ├── google.py            # Google
│   ├── azure.py             # Azure
│   ├── amazon.py            # Amazon
│   ├── baidu.py             # Baidu
│   ├── custom_llm.py        # Custom OpenAI-compatible service (Ollama, LM Studio…)
│   ├── deepseek.py          # DeepSeek (LLM)
│   └── openai_compatible.py # OpenAI 兼容接口基类
├── smart_translation_controller.py # SmartTranslationController — one-hotkey text probe and popup routing
├── translation_popup.py     # TranslationPopup — compact popup (selected text / typed input)
├── deepl_service.py         # DeepLService / TranslationThread — legacy async DeepL API calls
├── languages.py             # SupportedLanguages — supported language list & codes
├── translation_manager.py   # TranslationManager — translation window manager (singleton)
├── translation_dialog.py    # TranslationDialog — translation result window
└── ui/
    ├── dialog.py            # Translation dialog UI
    └── widgets.py           # Translation widgets
```

</details>

**Core Features:**
- Pluggable multi-provider architecture (DeepL / Google / Azure / Amazon) with a unified registry
- One hotkey: probes selected text and routes it to the popup
- Compact popup with selected-text and typed-input modes
- Async translation that never blocks the UI
- Copyable results

---

### translations/ — Language Resources

<details>
<summary>Expand directory structure</summary>

```text
translations/
├── app_zh.xml / app_en.xml  # Chinese / English source files
├── app_ja.xml / app_ko.xml  # Japanese / Korean source files
└── app_*.xml.qm             # compiled Qt binaries (e.g. app_zh.xml.qm)
```

</details>

`.xml` = editable source files, `*.xml.qm` = compiled Qt runtime files. Run `compile_translations.py` after modification.

---

### ui/ — UI Module

Common UI component library.

<details>
<summary>Expand directory structure</summary>

```text
ui/
├── toolbar.py               # Toolbar / _DragHandle — draggable toolbar base class
├── toolbar_layout.py        # screenshot toolbar button layout (order / visibility): normalize, load, save
├── toolbar_layout_dialog.py # ToolbarLayoutDialog — screenshot toolbar layout editor
├── reorderable_rows.py      # DragGrip / DraggableRow / ReorderableRowList — drag-to-reorder rows
├── tray_menu.py             # TrayMenu — system tray menu
├── screenshot_window.py     # ScreenshotWindow — full-screen capture window (region drawing)
├── dialogs.py               # StandardDialog — confirm, warning, info, error dialogs
├── toast.py                 # Toast — one-line hint by the cursor that never takes focus
├── magnifier.py             # MagnifierOverlay — pixel-level magnifier
├── color_picker_dialog.py   # ColorPickerDialog — custom HSV color picker
├── color_picker_button.py   # ColorPickerButton — color selection button
├── hotkey_edit.py           # HotkeyEdit — global hotkey editor
├── inapp_key_edit.py        # InAppKeyEdit — in-app shortcut editor
├── key_chip.py              # KeyChipLineEdit / StatusIcon — shared shortcut chip look
├── mask_overlay.py          # mask overlay layer
├── selection_overlay.py     # SelectionOverlayWidget — selection chrome layer above the mask
├── base_settings_panel.py   # BaseSettingsPanel / StepperWidget — settings panel base class
├── paint_settings_panel.py  # PaintSettingsPanel — brush settings panel
├── shape_settings_panel.py  # ShapeSettingsPanel — shape settings panel
├── text_settings_panel.py   # TextSettingsPanel — text settings panel
├── arrow_settings_panel.py  # ArrowSettingsPanel — arrow settings panel
├── number_settings_panel.py # number tool settings panel
├── mosaic_settings_panel.py # mosaic tool settings panel
│
├── fluent_lite/             # Fluent-style lightweight component library
│   ├── buttons.py / cards.py / icons.py / inputs.py  # buttons, cards, icons, inputs
│   ├── labels.py / navigation.py / segmented.py      # labels, navigation, segmented controls
│   ├── switch.py / theme.py / titlebar.py            # switch, theme, title bar
│   ├── frameless.py         # frameless windows
│   └── text_context_menu.py # text context menu
│
├── settings_ui/             # Application settings dialog
│   ├── dialog.py            # SettingsDialog — tabbed settings dialog
│   ├── components.py        # SettingCardGroup / ToggleSwitch — setting components
│   ├── page_appearance.py   # Appearance settings (theme, language)
│   ├── page_capture.py      # Capture settings
│   ├── page_quick_actions.py # Quick actions settings (skip confirm or result windows)
│   ├── color_format_dialog.py # ColorFormatDialog — magnifier color format editor
│   ├── page_clipboard.py    # Clipboard settings
│   ├── page_hotkey.py       # Hotkey settings
│   ├── page_translation.py  # Translation settings
│   ├── provider_fields.py   # 服务商字段的读写（按声明）
│   ├── page_log.py          # Log settings
│   ├── page_developer.py    # Developer settings
│   ├── page_misc.py         # Miscellaneous settings
│   ├── page_about.py        # About page
│   └── mock_config.py       # MockConfig — mock config for testing
│
├── welcome/                 # First-run welcome wizard (6-page guided setup)
│   ├── wizard.py            # WelcomeWizard — wizard main window
│   ├── base_page.py         # BasePage — wizard page base class
│   ├── page1_welcome.py     # Welcome page
│   ├── page2_screenshot.py  # Screenshot hotkey setup page
│   ├── page3_clipboard.py   # Clipboard hotkey setup page
│   ├── page5_translation.py # Translation feature intro page
│   ├── page6_finish.py      # Finish page
│   └── page_hotkeys.py      # Global hotkey page — six hotkeys share one conflict domain, set and validated together
│
└── selection_info/          # Selection info UI
    ├── controller.py        # Selection info controller
    ├── panel.py             # Selection info panel (dimensions, coordinates)
    ├── hook_manager.py      # Hook manager
    ├── border_shadow.py     # Selection border shadow effect
    ├── lock_ratio.py        # Aspect ratio lock
    └── rounded_corners.py   # Rounded corner capture
```

</details>

---

### tests/ — Test Module

<details>
<summary>Expand directory structure</summary>

```text
tests/
├── conftest.py              # pytest configuration and common fixtures
├── pytest.ini               # pytest run configuration
├── run_tests.py             # test runner script
├── test_drawing_tools.py    # drawing tool tests (pen/rect/arrow/text/number/highlighter)
├── test_mosaic_tool.py      # mosaic tool tests
├── test_spotlight.py        # spotlight tests
├── test_functional_handles.py # edit handle tests
├── test_capture_service.py  # capture service tests
├── test_clipboard_api.py    # clipboard public API tests
├── test_clipboard_manage_dialog.py # clipboard management window tests
├── test_frame_recorder.py   # GIF frame recorder tests
├── test_playback_engine.py  # GIF playback engine tests
├── test_ocr_text_layer.py   # OCR text layer tests
├── test_pin_window_zoom.py  # pin window zoom tests
├── test_smart_translation.py # smart translation tests
├── test_translation_architecture.py # translation provider architecture tests
├── test_stitch_dedup.py     # long-stitch dedup tests
├── test_settings_dialog_state.py # settings dialog state tests
├── test_welcome_translation.py # welcome wizard translation page tests
└── … (70+ additional unit & integration test files)
```

</details>
