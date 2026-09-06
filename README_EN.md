[中文](README.md) | **[English](README_EN.md)** | [日本語](README_JA.md)

# Screenshot & Clipboard Manager — jietuba

## Overview

A screenshot and clipboard management application built with PySide6 and Rust. Supports area capture, smart window detection, GIF recording, long screenshot stitching, OCR text recognition, image pinning, translation, mosaic blur, PDF export, and a full clipboard history management system.

---

## Prerequisites

This project depends on 4 custom Rust libraries. **You must install these packages before running the program.**

### One-Click Setup (Recommended)

Double-click [setup.bat](setup.bat) in the project root. It will create a virtual environment, install the Python dependencies, and let you choose where the Rust packages come from:

- Use the `.whl` files already bundled in the repo (default)
- Or download the latest ones from the [GitHub Release](https://github.com/1003129155/jietuba/releases/tag/rust-libs-v1)

You can then choose to launch the application immediately.

### Manual Setup

If you'd rather not use the script, follow these steps:

### 1. Create and Activate Python 3.11 Virtual Environment

```bash
python -m venv venv311
# Windows:
venv311\Scripts\activate
```

### 2. Install Python Dependencies

```bash
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

### 3. Install Custom Rust Packages (Required)

There are two sources for these 4 packages, pick either one:

**Option A: Use the `.whl` files bundled in the repo** (from the project root)

```bash
python -m pip install gifrecorder-0.2.1-cp311-cp311-win_amd64.whl longstitch-0.3.11-cp311-cp311-win_amd64.whl pyclipboard-0.3.14-cp311-cp311-win_amd64.whl ppocr_rust-0.1.1-cp311-cp311-win_amd64.whl
```

**Option B: Download from GitHub Release** (if the files are missing or you want the latest build)

Download the matching files from the [Releases page](https://github.com/1003129155/jietuba/releases/tag/rust-libs-v1), then `pip install` them the same way.

| Package | Version | Description |
|---------|---------|-------------|
| `gifrecorder` | 0.2.1 | GIF/video composition encoder |
| `longstitch` | 0.3.11 | Long screenshot stitching algorithm |
| `pyclipboard` | 0.3.14 | Low-level clipboard operations |
| `ppocr_rust` | 0.1.1 | PP-OCR (PaddleOCR) ONNX text recognition (pure Rust + ONNX Runtime, needs det/rec models) |

> **Note:** These `.whl` files are for Windows x86_64 + Python 3.11 only. Do not install into the global Python environment.

> **OCR models:** `ppocr_rust` requires the PP-OCR ONNX models in the `models/` folder (`PP-OCRv6_det_small.onnx` + `PP-OCRv6_rec_small.onnx`), already bundled in the repo. When packaged for release, place `models/` next to the exe.

**Development/Build Dependencies (Optional):**

```bash
python -m pip install -r requirements-dev.txt
```

### 4. Run the Application

```bash
cd main
python main_app.py
```

---

## Directory Structure

```
# Project root
├── README.md / README_EN.md / README_JA.md             # Chinese, English, and Japanese documentation
├── pyproject.toml                                      # Python project metadata and dependency declarations
├── requirements.txt                                   # Runtime dependencies
├── requirements-dev.txt                               # Test and build dependencies
├── build_with_ocr_onefile.py                           # PyInstaller one-file build script
├── gifrecorder-0.2.1-cp311-cp311-win_amd64.whl       # GIF recorder Rust pre-built package
├── longstitch-0.3.11-cp311-cp311-win_amd64.whl        # Long-stitch Rust pre-built package
├── pyclipboard-0.3.14-cp311-cp311-win_amd64.whl      # Clipboard Rust pre-built package
├── ppocr_rust-0.1.1-cp311-cp311-win_amd64.whl        # OCR Rust pre-built package
│
├── main/                    # Python main program
│   ├── main_app.py          # App entry point: system tray, global hotkeys, lifecycle management
│   ├── compile_translations.py  # Translation compiler (.xml → .qm)
│   ├── scripts/             # Helper scripts — translation provider comparison
│   │
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

---

## Module Details

### canvas/ — Canvas Module

Graphics editing canvas system with scene management, view rendering, item selection, and undo/redo.
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

```
canvas/
├── __init__.py
├── scene.py                 # CanvasScene — canvas scene, extends QGraphicsScene
├── view.py                  # CanvasView — canvas view, extends QGraphicsView
├── selection_model.py       # SelectionModel — manages selected graphics items
├── undo.py                  # CommandUndoStack — undo/redo stack (add, delete, batch, edit commands)
├── smart_edit_controller.py # SmartEditController — handles selection/edit mode switching
├── handle_editor.py         # LayerEditor / EditHandle — control point drag editing
└── items/
    ├── drawing_items.py     # StrokeItem / RectItem / EllipseItem / ArrowItem / TextItem / NumberItem
    ├── background_item.py   # BackgroundItem — selection area background
    ├── mosaic_item.py       # MosaicItem — pixel mosaic item
    └── selection_item.py    # SelectionItem — selection boundary display
```

---

### capture/ — Capture Module

Screen capture and smart window detection.

```
capture/
├── capture_service.py       # CaptureService — core screenshot logic
└── window_finder.py         # WindowFinder — smart window selection, cursor-based detection
```

---

### clipboard/ — Clipboard Management Module

Ditto-like clipboard history manager, now organized into controllers, core, services, and ui layers.
Supports text, images, HTML, files, a dedicated three-pane management window, and pin creation from history items.
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

```
clipboard/
├── __init__.py
├── controllers/             # Control layer — history loading, paste flow, menus, selection state
│   ├── clipboard_controller.py   # ClipboardController — loading, pasting, context menu logic
│   ├── selection_manager.py      # SelectionManager — list selection state
│   └── __init__.py
├── core/                    # Data layer — pyclipboard wrapper, models, group types
│   ├── manager.py           # ClipboardManager — storage, monitoring, and paste API
│   ├── models.py            # ClipboardItem / Group models
│   ├── enums.py             # GroupType definitions
│   └── __init__.py
├── services/                # Service layer — file payloads, group rules, import/export, save logic
│   ├── file_payload_service.py   # file payload JSON + legacy format compatibility
│   ├── group_service.py          # group icon/name/delete helpers
│   ├── import_export_service.py  # CSV import/export for text items
│   └── manage_dialog_service.py  # persistence logic for the management window
├── ui/
│   ├── dialogs/
│   │   └── manage_dialog.py      # three-pane management window for groups and content
│   ├── forms/
│   │   ├── group_form.py
│   │   ├── text_content_form.py
│   │   ├── file_content_form.py
│   │   ├── import_export_form.py
│   │   └── group_icon_picker.py
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
│   │   ├── draggable_list_widget.py
│   │   ├── group_bar.py
│   │   ├── item_delegate.py
│   │   ├── item_widget.py
│   │   └── preview_popup.py
│   └── windows/
│       ├── clipboard_window.py   # history window, search, preview, quick paste
│       └── pin_window.py         # create pins from history items
```

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

```
core/
├── bootstrap.py             # PreloadManager — startup bootstrap, env init, DPI, single instance
├── logger.py                # Logger — file + console logging (debug/info/warning/error/exception)
├── crash_handler.py         # install_crash_hooks() — global exception catching
├── resource_manager.py      # ResourceManager — SVG/image resource loading
├── theme.py                 # ThemeManager — application theme colors
├── i18n.py                  # I18nManager / XmlTranslator / tr() — internationalization
├── shortcut_manager.py      # HotkeySystem / ShortcutManager — global & in-app hotkeys
├── save.py                  # SaveService — file save service (auto naming, high-quality PDF output)
├── export.py                # ExportService — image export
├── clipboard_utils.py       # copy_image_to_clipboard() — copy images to system clipboard
├── platform_utils.py        # DPI awareness, AppUserModelID, Windows API utilities
├── qt_utils.py              # safe_disconnect() — Qt signal safe disconnect
├── log_translations/        # per-module log text translation helpers
└── constants.py             # Global constants (fonts, paths, etc.)
```

---

### gif/ — GIF Recording Module

Screen recording, editing, playback, and export to GIF/video.
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
```
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

---

### ocr/ — OCR Module

Text recognition management powered by PP-OCR.

<img width="580" height="505" alt="image" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />

```
ocr/
└── ocr_manager.py           # OCRManager — text recognition via ppocr_rust (PP-OCR)
```

- Powered by the ppocr_rust engine (pure Rust + ONNX Runtime, PP-OCR det + rec); inference runs on native threads without blocking the UI
- Chinese/English/Japanese recognition
- Singleton pattern, unified recognition interface

---

### pin/ — Pin Module

Pin screenshots on screen with editing, zoom, OCR, and translation.

<img width="737" height="657" alt="image" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />

```
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

---

### settings/ — Settings Module

```
settings/
└── tool_settings.py         # ToolSettingsManager / ToolSettings — tool color, size, hotkey config
```

---

### stitch/ — Long Screenshot Stitching Module
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)

```
stitch/
├── jietuba_long_stitch_unified.py   # Stitching interface (calls the Rust longstitch)
├── scroll_window.py                 # ScrollCaptureWindow — scroll capture window
└── scroll_toolbar.py                # Scroll capture toolbar
```

---

### tools/ — Drawing Tools Module

```
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
├── cursor.py                # CursorTool — cursor/selection
├── eraser.py                # EraserTool — eraser
└── cursor_manager.py        # CursorManager — cursor style manager
```

---

### translation/ — Translation Module

Multi-provider translation service supporting DeepL / Google / Azure / Amazon.

```
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
│   └── amazon.py            # Amazon
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

**Core Features:**
- Pluggable multi-provider architecture (DeepL / Google / Azure / Amazon) with a unified registry
- One hotkey: probes selected text and routes it to the popup
- Compact popup with selected-text and typed-input modes
- Async translation that never blocks the UI
- Copyable results

---

### translations/ — Language Resources

```
translations/
├── app_zh.xml / app_en.xml  # Chinese / English source files
├── app_ja.xml / app_ko.xml  # Japanese / Korean source files
└── app_*.xml.qm             # compiled Qt binaries (e.g. app_zh.xml.qm)
```

`.xml` = editable source files, `*.xml.qm` = compiled Qt runtime files. Run `compile_translations.py` after modification.

---

### ui/ — UI Module

Common UI component library.

```
ui/
├── toolbar.py               # Toolbar / _DragHandle — draggable toolbar base class
├── tray_menu.py             # TrayMenu — system tray menu
├── screenshot_window.py     # ScreenshotWindow — full-screen capture window (region drawing)
├── dialogs.py               # StandardDialog — confirm, warning, info, error dialogs
├── magnifier.py             # MagnifierOverlay — pixel-level magnifier
├── color_picker_dialog.py   # ColorPickerDialog — custom HSV color picker
├── color_picker_button.py   # ColorPickerButton — color selection button
├── hotkey_edit.py           # HotkeyEdit — global hotkey editor
├── inapp_key_edit.py        # InAppKeyEdit — in-app shortcut editor
├── mask_overlay.py          # mask overlay layer
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
│   ├── page_clipboard.py    # Clipboard settings
│   ├── page_hotkey.py       # Hotkey settings
│   ├── page_translation.py  # Translation settings
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
│   ├── page4_smart_select.py # Smart select intro page
│   ├── page5_translation.py # Translation feature intro page
│   └── page6_finish.py      # Finish page
│
└── selection_info/          # Selection info UI
    ├── controller.py        # Selection info controller
    ├── panel.py             # Selection info panel (dimensions, coordinates)
    ├── hook_manager.py      # Hook manager
    ├── border_shadow.py     # Selection border shadow effect
    ├── lock_ratio.py        # Aspect ratio lock
    └── rounded_corners.py   # Rounded corner capture
```

---

### tests/ — Test Module

```
tests/
├── conftest.py              # pytest configuration and common fixtures
├── pytest.ini               # pytest run configuration
├── run_tests.py             # test runner script
├── test_drawing_tools.py    # drawing tool tests (pen/rect/arrow/text/number/highlighter)
├── test_mosaic_tool.py      # mosaic tool tests
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
