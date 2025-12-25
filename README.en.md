<div align="center">

[English](README.en.md) | [日本語](README.ja.md) | [简体中文](README.md)

</div>

---

# Jietuba Screenshot Tool
A Windows screenshot tool with image editing, OCR recognition, and long screenshot.


## 📸 Features

### Core Features

- **🎯 Smart Screenshot**
  - Hotkey screenshot (Default: Ctrl+1)
  - Smart window/control recognition
  - Multi-monitor support
  - Full screen screenshot

- **🎨 Rich Editing Tools**
  - Pen: Free drawing -- Hold Shift for straight lines
  - Line/Arrow: Precise annotations
  - Rectangle/Ellipse: Fill and outline support
  - Text: Font, color, and size adjustment
  - Highlighter: Marking -- Hold Shift for straight lines
  - Eraser: Precise erasing
  - Number annotation: Auto-increment -- Shift+Scroll to change number

- **📌 Pin Feature**
  - Pin screenshots to desktop
  - Drag and resize -- Scroll to zoom
  - Re-edit support

- **🔤 OCR Text Recognition on Pinned Images (Automatic)**
  - Based on RapidOCR engine (Local)
  - Supports Chinese, English, and Japanese
  - Real-time region recognition
  - Automatic layout processing
  - Editable text layer

- **📜 Long Screenshot**
  - Smart scroll screenshot
  - Custom stitching method
  - Auto-deduplication and stitching in Rust for efficiency

**Environment Setup**

① Install all required packages in the virtual environment via pip

② Install the custom Rust package in the virtual environment:
pip install jietuba_rust-0.3.0-cp39-cp39-win_amd64.whl  

③ Replace the default OCR model in rapidocr installed in the virtual environment:
venv/Lib/site-packages/rapidocr/
├── default_models.yaml    
├── config.yaml            
└── models/                # 🔥 Replace all files in this folder (provided)


④ python main_app.py

### First Use

1. After launching the program, an icon will appear in the system tray
2. Right-click the tray icon to:
   - Start screenshot (or press Ctrl+1)
   - Open settings
   - Exit program
3. After taking a screenshot, various editing tools are available
4. Press `Enter` to save, `ESC` to cancel

### Directory Structure

```text
jietu/
├── main/                    # Main source code directory
│   ├── main_app.py                # Application entry point
│   ├── requirements_no_ocr.txt    # Basic version dependencies
│   ├── requirements_with_ocr.txt  # Full version dependencies
│   ├── OCR_SETUP_GUIDE.md         # OCR setup guide
│   │
│   ├── canvas/                    # Canvas system (Core drawing engine)
│   │   ├── model.py              # Data models (Drawing items, Selection area)
│   │   ├── scene.py              # Scene management (QGraphicsScene)
│   │   ├── view.py               # View control (Zoom, Drag, Smart selection)
│   │   ├── toolbar_adapter.py    # Toolbar adapter
│   │   ├── layer_editor.py       # Layer editor
│   │   ├── export.py             # Image export
│   │   ├── undo.py               # Undo/Redo system
│   │   ├── snap_system.py        # Snap assist system
│   │   ├── cursor_decision.py    # Cursor state management
│   │   ├── smart_edit_controller.py  # Smart edit controller
│   │   └── items/                # Drawing items (Shape elements)
│   │       ├── base.py           # Base shape item
│   │       ├── pen.py            # Pen shape item
│   │       ├── arrow.py          # Arrow shape item
│   │       ├── rect.py           # Rectangle shape item
│   │       ├── ellipse.py        # Ellipse shape item
│   │       ├── text.py           # Text shape item
│   │       ├── mosaic.py         # Mosaic shape item
│   │       └── ...
│   │
│   ├── capture/                   # Screenshot capture module
│   │   ├── capture_service.py    # Screenshot service (Multi-monitor support)
│   │   ├── window_finder.py      # Smart window recognition (Windows API)
│   │   └── SMART_SELECTION.md    # Smart selection documentation
│   │
│   ├── tools/                     # Drawing toolset
│   │   ├── base.py               # Tool base class
│   │   ├── controller.py         # Tool controller
│   │   ├── pen.py                # Pen tool
│   │   ├── arrow.py              # Arrow tool
│   │   ├── rect.py               # Rectangle tool
│   │   ├── ellipse.py            # Ellipse tool
│   │   ├── text.py               # Text tool
│   │   ├── highlighter.py        # Highlighter tool
│   │   ├── eraser.py             # Eraser tool
│   │   ├── number.py             # Number tool
│   │   ├── cursor.py             # Cursor tool (Select & Move)
│   │   ├── action.py             # Action tool (Undo/Redo)
│   │   └── cursor_manager.py     # Cursor manager
│   │
│   ├── pin/                       # Pin module
│   │   ├── pin_window.py         # Pin window
│   │   ├── pin_manager.py        # Pin manager
│   │   ├── pin_canvas.py         # Pin canvas
│   │   ├── pin_canvas_view.py    # Pin view
│   │   ├── pin_canvas_renderer.py # Pin renderer
│   │   ├── pin_toolbar.py        # Pin toolbar
│   │   ├── ocr_text_layer.py     # OCR text layer
│   │   └── pin_mock_scene.py     # Mock scene (for pin)
│   │
│   ├── ocr/                       # OCR recognition module
│   │   ├── ocr_manager.py        # OCR manager
│   │   ├── OCR_INTEGRATION.md    # OCR integration documentation
│   │   └── ocr_model_checker.py  # (in core/) Model checker
│   │
│   ├── stitch/                    # Long screenshot stitching module
│   │   ├── scroll_window.py      # Scroll screenshot window
│   │   ├── jietuba_long_stitch.py       # Stitching algorithm (Smart selection)
│   │   ├── jietuba_long_stitch_rust.py  # Rust accelerated version
│   │   └── jietuba_long_stitch_unified.py # Unified interface
│   │
│   ├── ui/                        # User interface module
│   │   ├── screenshot_window.py  # Screenshot window (Main window)
│   │   ├── settings_window.py    # Settings window
│   │   ├── toolbar.py            # Toolbar
│   │   ├── color_board.py        # Color picker
│   │   ├── size_slider.py        # Size slider
│   │   └── ...
│   │
│   ├── core/                      # Core functionality module
│   │   ├── hotkey_system.py      # Global hotkey system
│   │   ├── resource_manager.py   # Resource manager
│   │   ├── logger.py             # Logging system
│   │   ├── save.py               # Save functionality
│   │   └── ocr_model_checker.py  # OCR model check
│   │
│   └── settings/                  # Settings management module
│       └── tool_settings.py      # Tool settings manager
│
├── packaging/                     # Packaging scripts
│   ├── build_no_ocr.py           # Build without OCR
│   ├── build_with_ocr.py         # Build full version
│   ├── build_no_ocr_onefile.py   # Single file without OCR
│   └── build_with_ocr_onefile.py # Single file full version
│
├── svg/                           # SVG icon resources
├── models/                        # OCR model files
```

<div align="center">

**If this project was helpful, please give it a ⭐ Star!**

Made with ❤️ by RiJyaaru

</div>
