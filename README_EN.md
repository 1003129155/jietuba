[中文](README.md) | **[English](README_EN.md)** | [日本語](README_JA.md)

# Screenshot & Clipboard Manager — jietuba

## Overview

A screenshot and clipboard management application built with PySide6 and RUST. Supports area capture, smart window detection, GIF recording, long screenshot stitching, OCR text recognition, image pinning, translation, and a full clipboard history management system.

---

## Prerequisites

This project depends on 4 custom Rust libraries. **You must install these packages before running the program.**

### 1. Create and Activate Python 3.11 Virtual Environment

```bash
python -m venv venv311
# Windows:
venv311\Scripts\activate
```

### 2. Install Custom Rust Packages (Required)

```bash
pip install gifrecorder-0.2.1-cp311-cp311-win_amd64.whl
pip install longstitch-0.3.8-cp311-cp311-win_amd64.whl
pip install pyclipboard-0.3.10-cp311-cp311-win_amd64.whl
pip install windows_media_ocr-0.3.1-cp311-cp311-win_amd64.whl
```

| Package | Version | Description |
|---------|---------|-------------|
| `gifrecorder` | 0.2.1 | GIF/video composition encoder |
| `longstitch` | 0.3.8 | Long screenshot stitching algorithm |
| `pyclipboard` | 0.3.10 | Low-level clipboard operations |
| `windows_media_ocr` | 0.3.1 | Windows Media OCR API & oneocr.dll wrapper |

> **Note:** These `.whl` files are for Windows x86_64 + Python 3.11 only. Do not install into the global Python environment.

### 3. Install Python Dependencies

```bash
pip install PySide6==6.11.0
pip install PySide6-Fluent-Widgets==1.11.1
pip install PySideSix-Frameless-Window==0.8.1
pip install pillow==12.1.1
pip install mss==10.1.0
pip install pynput==1.8.1
pip install pywin32==311
pip install darkdetect==0.8.0
pip install emoji==2.15.0
pip install av==17.0.0
pip install colorama==0.4.6
```

**Development/Build Dependencies (Optional):**

```bash
pip install pytest==9.0.2 pytest-qt==4.5.0   # Testing
pip install pyinstaller==6.17.0              # Packaging
pip install maturin==1.12.6                  # Rust library building
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
├── gifrecorder-0.2.1-cp311-cp311-win_amd64.whl       # GIF recorder Rust pre-built package
├── longstitch-0.3.8-cp311-cp311-win_amd64.whl        # Long-stitch Rust pre-built package
├── pyclipboard-0.3.10-cp311-cp311-win_amd64.whl      # Clipboard Rust pre-built package
├── windows_media_ocr-0.3.1-cp311-cp311-win_amd64.whl # OCR Rust pre-built package
│
├── main/                    # Python main program
│   ├── main_app.py          # App entry point: system tray, global hotkeys, lifecycle management
│   ├── compile_translations.py  # Translation compiler (.xml → .qm)
│   │
│   ├── canvas/              # Canvas module — graphics editing core
│   ├── capture/             # Capture module — screen capture & window detection
│   ├── clipboard/           # Clipboard module — history, groups, search
│   ├── core/                # Core module — bootstrap, logging, resources, theme, i18n, hotkeys
│   ├── gif/                 # GIF module — screen recording, editing, playback, export
│   ├── ocr/                 # OCR module — multi-engine text recognition
│   ├── pin/                 # Pin module — pinned screenshots, editing, OCR, translation
│   ├── settings/            # Settings module — unified configuration management
│   ├── stitch/              # Stitch module — scroll capture, auto-stitching
│   ├── tools/               # Tools module — pen, rect, arrow, text, etc.
│   ├── translation/         # Translation module — DeepL API service
│   ├── translations/        # Language resources — Chinese/English/Japanese
│   ├── ui/                  # UI module — common UI component library
│   └── tests/               # Tests module — unit tests & integration tests
│
├── rust_libs/               # Rust library source code (buildable from source)
│   ├── gifrecorder/         # GIF/video composition encoder source
│   ├── longstitch/          # Long screenshot stitching algorithm source
│   ├── pyclipboard/         # Low-level clipboard operations source
│   └── windows_media_ocr/   # Windows Media OCR wrapper source
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

Ditto-like clipboard history manager with group management, supporting text, images, files, etc.
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

```
clipboard/
├── data_manager.py          # ClipboardManager / ClipboardItem / Group — database storage & retrieval
├── window.py                # ClipboardWindow — clipboard history main window
├── data_controller.py       # ClipboardController — business logic, shortcuts, context menu
├── data_setting.py          # ManageDialog — group management dialog
├── interaction.py           # SelectionManager — list selection state management
├── item_widget.py           # ClipboardItemWidget — single history item display
├── item_delegate.py         # ClipboardItemDelegate — custom list item rendering
├── preview_popup.py         # PreviewPopup — large image/text preview popup
├── themes.py                # ThemeManager / Theme / ThemeColors — theme management
├── theme_styles.py          # ThemeStyleGenerator — CSS stylesheet generator
├── pin_window.py            # Create pins from clipboard items
├── emoji_data.py            # Emoji data management
├── frameless_mixin.py       # FramelessMixin — borderless window mixin
└── setting_panel.py         # Clipboard settings panel
```

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
├── save.py                  # SaveService — file save service
├── export.py                # ExportService — image export
├── clipboard_utils.py       # copy_image_to_clipboard() — copy images to system clipboard
├── platform_utils.py        # DPI awareness, AppUserModelID, Windows API utilities
├── qt_utils.py              # safe_disconnect() — Qt signal safe disconnect
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

Multi-engine text recognition management.

```
ocr/
└── ocr_manager.py           # OCRManager — Windows Media OCR + oneocr dual engine
```

- Supports Windows Media OCR (lightweight, built-in) and oneocr high-precision engine (via Rust FFI)
- Chinese/English/Japanese recognition
- Singleton pattern, unified recognition interface

---

### pin/ — Pin Module

Pin screenshots on screen with editing, zoom, OCR, and translation.

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
├── jietuba_long_stitch.py           # Core stitching algorithm
├── jietuba_long_stitch_unified.py   # Unified stitching interface
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
├── highlighter.py           # HighlighterTool — highlighter/mosaic
├── cursor.py                # CursorTool — cursor/selection
├── eraser.py                # EraserTool — eraser
└── cursor_manager.py        # CursorManager — cursor style manager
```

---

### translation/ — Translation Module

DeepL API-based text translation.

```
translation/
├── deepl_service.py         # DeepLService / TranslationThread — async DeepL API calls
├── languages.py             # SupportedLanguages — DeepL supported language list & codes
├── translation_manager.py   # TranslationManager — translation window manager (singleton)
├── translation_dialog.py    # TranslationDialog — translation result window
└── ui/
    ├── dialog.py            # Translation dialog UI
    └── widgets.py           # Translation widgets
```

---

### translations/ — Language Resources

```
translations/
├── app_zh.xml / app_zh.qm  # Chinese
├── app_en.xml / app_en.qm  # English
└── app_ja.xml / app_ja.qm  # Japanese
```

`.xml` = editable source files, `.qm` = compiled Qt binary files. Run `compile_translations.py` after modification.

---

### ui/ — UI Module

Common UI component library.

```
ui/
├── toolbar.py               # Toolbar / _DragHandle — draggable toolbar base class
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
├── test_undo_stack.py       # Undo stack tests
├── test_selection_model.py  # Selection model tests
├── test_clipboard_data.py   # Clipboard data layer tests
├── test_clipboard_themes.py # Clipboard theme system tests
├── test_core_utils.py       # Core utilities tests
├── test_crash_handler.py    # Crash handler tests
├── test_emoji_data.py       # Emoji data tests
├── test_gif_data.py         # GIF data structure tests
├── test_i18n.py             # i18n tests
├── test_resource_manager.py # Resource manager tests
├── test_save_service.py     # Save service tests
├── test_stitch_algorithm.py # Stitching algorithm tests
├── test_theme_manager.py    # Theme manager tests
├── test_tool_settings.py    # Tool settings tests
└── test_tools_base.py       # Tool base class tests
```
