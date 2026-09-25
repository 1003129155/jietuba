# -*- coding: utf-8 -*-
"""截图设置页 — Fluent Design"""
import importlib.util

from PySide6.QtWidgets import QWidget, QVBoxLayout, QScrollArea
from core.ui_scale import dialog_scaled
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel,
    PushButton,
)
from settings import color_formats
from settings.tool_settings import SMART_SELECTION_MODES
from .components import SettingCardGroup
from core.ui_theme import set_own_style


def create_capture_page(dialog) -> QWidget:
    """截图設定 ─ 智能选区 + 保存设置 + 放大镜 + 钉图"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    # ── 智能选区 ──────────────────────────────────────
    grp_smart = SettingCardGroup(dialog.tr("Smart Selection"), view)

    # 三档做成一个下拉框而不是"总开关 + 下钻子开关"：配置本来就只有
    # off/window/element 三个值，拆成两个开关会多出"总开关关着、下钻开着"
    # 这个非法组合，只能再用 setEnabled 联动去遮。
    mode_card = FSettingCard(
        FluentIcon.CAMERA,
        dialog.tr("Detection Mode"),
        dialog.tr(
            "Snap the selection to the window or control under the cursor. "
            "Control detection reads the accessibility tree and takes longer on "
            "large windows."
        ),
        parent=grp_smart,
    )
    dialog.smart_mode_combo = ComboBox(mode_card)
    for label, mode in ((dialog.tr("Off"), "off"),
                        (dialog.tr("Window"), "window"),
                        (dialog.tr("Control"), "element")):
        dialog.smart_mode_combo.addItem(label, userData=mode)
    dialog.smart_mode_combo.setCurrentIndex(
        SMART_SELECTION_MODES.index(dialog.config_manager.get_smart_selection_mode())
    )
    mode_card.addControl(dialog.smart_mode_combo)
    grp_smart.addSettingCard(mode_card)

    smart_anim_card = SwitchSettingCard(
        FluentIcon.SYNC,
        dialog.tr("Selection Switch Animation"),
        dialog.tr("Slide the selection when it moves from one window or control to another."),
        parent=grp_smart,
    )
    smart_anim_card.setChecked(dialog.config_manager.get_smart_selection_animation())
    dialog.smart_animation_toggle = smart_anim_card
    grp_smart.addSettingCard(smart_anim_card)

    layout.addWidget(grp_smart)

    # ── 截图保存 ──────────────────────────────────────
    grp_save = SettingCardGroup(dialog.tr("Save Settings"), view)

    save_card = SwitchSettingCard(
        FluentIcon.SAVE,
        dialog.tr("Auto-save Screenshots"),
        dialog.tr("Automatically saves as file when capturing."),
        parent=grp_save,
    )
    save_card.setChecked(dialog.config_manager.get_screenshot_save_enabled())
    dialog.save_toggle = save_card
    grp_save.addSettingCard(save_card)

    # 写入文件路径到剪贴板（CF_HDROP）。是否生效取决于自动保存截图是否
    # 开启（没有落盘就没有路径可写），这个依赖在 ActionTools 里按需读取
    # 判断，不在 UI 上做联动禁用——开关各自独立存储，互不覆盖，重新打开
    # 自动保存时也不需要再点一次这个开关。
    file_ref_card = SwitchSettingCard(
        FluentIcon.COMMAND_PROMPT,
        dialog.tr("Write File Path to Clipboard"),
        dialog.tr(
            "Lets tools that only recognize a file path (e.g. some terminal apps) "
            "paste the screenshot too. Requires Auto-save Screenshots to be enabled."
        ),
        parent=grp_save,
    )
    file_ref_card.setChecked(dialog.config_manager.get_clipboard_file_reference_enabled())
    dialog.clipboard_file_reference_toggle = file_ref_card
    grp_save.addSettingCard(file_ref_card)

    # 保存路径。路径本身当作卡片描述，按钮走控件列，和同组其它卡片对齐。
    path_card = FSettingCard(
        FluentIcon.FOLDER,
        dialog.tr("Save Folder"),
        dialog.config_manager.get_screenshot_save_path(),
        parent=grp_save,
    )
    dialog.save_path_lbl = path_card.contentLabel

    btn_change = PushButton(dialog.tr("Change"), path_card)
    btn_change.clicked.connect(dialog._change_save_dir)
    btn_open = PushButton(dialog.tr("Open"), path_card)
    btn_open.clicked.connect(dialog._open_save_dir)
    path_card.addControl(btn_change)
    path_card.addControl(btn_open)
    grp_save.addSettingCard(path_card)

    # 保存格式
    fmt_card = FSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Save Format"),
        dialog.tr("File format for auto-saved screenshots."),
        parent=grp_save,
    )
    dialog.screenshot_format_combo = ComboBox(fmt_card)
    dialog.screenshot_format_combo.addItem("PNG", userData="PNG")
    dialog.screenshot_format_combo.addItem("JPG", userData="JPG")
    dialog.screenshot_format_combo.addItem("BMP", userData="BMP")
    dialog.screenshot_format_combo.addItem("WebP", userData="WEBP")
    dialog.screenshot_format_combo.addItem("PDF", userData="PDF")
    _fmt_idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3, "PDF": 4}.get(
        dialog.config_manager.get_screenshot_format().upper(), 0
    )
    dialog.screenshot_format_combo.setCurrentIndex(_fmt_idx)
    fmt_card.addControl(dialog.screenshot_format_combo)
    grp_save.addSettingCard(fmt_card)

    layout.addWidget(grp_save)

    # ── 放大镜 ────────────────────────────────────────
    grp_magnifier = SettingCardGroup(dialog.tr("Magnifier"), view)

    magnifier_card = SwitchSettingCard(
        FluentIcon.SEARCH,
        dialog.tr("Show Magnifier"),
        dialog.tr("Follows the cursor while selecting, and picks the color under it."),
        parent=grp_magnifier,
    )
    magnifier_card.setChecked(dialog.config_manager.get_app_setting("magnifier_enabled"))
    dialog.magnifier_enabled_toggle = magnifier_card
    grp_magnifier.addSettingCard(magnifier_card)

    grid_card = SwitchSettingCard(
        FluentIcon.LAYOUT,
        dialog.tr("Pixel Grid"),
        dialog.tr("Outline each pixel in the magnified view. Hidden at low zoom, where the lines would cover the pixels."),
        parent=grp_magnifier,
    )
    grid_card.setChecked(dialog.config_manager.get_app_setting("magnifier_grid"))
    dialog.magnifier_grid_toggle = grid_card
    grp_magnifier.addSettingCard(grid_card)

    swatch_card = SwitchSettingCard(
        FluentIcon.BRUSH,
        dialog.tr("Color Swatch"),
        dialog.tr("Show the picked color in the top-right corner of the magnified view."),
        parent=grp_magnifier,
    )
    swatch_card.setChecked(dialog.config_manager.get_app_setting("magnifier_swatch"))
    dialog.magnifier_swatch_toggle = swatch_card
    grp_magnifier.addSettingCard(swatch_card)

    hint_card = SwitchSettingCard(
        FluentIcon.INFO,
        dialog.tr("Shortcut Hint"),
        dialog.tr("Show the color-copy shortcut under the readings. Off makes the magnifier shorter."),
        parent=grp_magnifier,
    )
    hint_card.setChecked(dialog.config_manager.get_app_setting("magnifier_hint"))
    dialog.magnifier_hint_toggle = hint_card
    grp_magnifier.addSettingCard(hint_card)

    # 格式可以勾选多条、还能自己加，一个下拉装不下，另开一个管理窗口。
    # 编辑结果先存在 dialog 上，点「应用」时才落盘。
    fmt_card = FSettingCard(
        FluentIcon.PALETTE,
        dialog.tr("Color Formats"),
        dialog.tr("Which formats the magnifier shows, and which one the copy shortcut uses."),
        parent=grp_magnifier,
    )
    dialog.magnifier_color_formats = color_formats.load(dialog.config_manager)
    manage_btn = PushButton(dialog.tr("Manage Color Formats"), fmt_card)
    manage_btn.clicked.connect(lambda: _manage_color_formats(dialog))
    dialog.magnifier_color_formats_btn = manage_btn
    fmt_card.addControl(manage_btn)
    grp_magnifier.addSettingCard(fmt_card)

    layout.addWidget(grp_magnifier)

    # ── 钉图 ──────────────────────────────────────────
    # 自动开工具栏和自动 OCR 都是「钉完图之后自动做什么」，归在一处。
    grp_pin = SettingCardGroup(dialog.tr("Pin Settings"), view)

    pin_toolbar_card = SwitchSettingCard(
        FluentIcon.PIN,
        dialog.tr("Auto-show Drawing Tools on Pin"),
        dialog.tr("On: Shows toolbar when mouse enters pinned window.")
        + "\n"
        + dialog.tr("Off: Show via right-click toolbar button."),
        parent=grp_pin,
    )
    pin_toolbar_card.setChecked(dialog.config_manager.get_pin_auto_toolbar())
    dialog.pin_auto_toolbar_toggle = pin_toolbar_card
    grp_pin.addSettingCard(pin_toolbar_card)

    # OCR 可用性检测：走 ocr 模块的官方多引擎检测（含 ppocr_rust / windows_media_ocr），
    # 而不是只看 windows_media_ocr —— 否则装了 ppocr_rust 也会误报“无 OCR 版本”。
    try:
        from ocr import is_ocr_available
        ocr_available = bool(is_ocr_available())
    except Exception:
        # 兜底：ocr 模块不可导入时，退回到最基础的探测
        ocr_available = (
            importlib.util.find_spec("ppocr_rust") is not None
            or importlib.util.find_spec("windows_media_ocr") is not None
        )
    ocr_card = SwitchSettingCard(
        FluentIcon.FONT,
        dialog.tr("Automatically recognize text after pinning"),
        dialog.tr(
            "On very low-end computers, OCR after pinning may cause a brief stutter. "
            "Turn off automatic recognition if needed."
        ),
        parent=grp_pin,
    )
    ocr_card.setChecked(
        dialog.config_manager.get_ocr_enabled() if ocr_available else False
    )
    if not ocr_available:
        ocr_card.setEnabled(False)
        ocr_card.setChecked(False)
    dialog.ocr_enable_toggle = ocr_card
    grp_pin.addSettingCard(ocr_card)

    if not ocr_available:
        no_ocr_card = FSettingCard(
            FluentIcon.INFO,
            dialog.tr("No OCR Version / OCR module not found"),
            parent=grp_pin,
        )
        grp_pin.addSettingCard(no_ocr_card)

    layout.addWidget(grp_pin)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Even with auto-save off, it will be copied to clipboard."),
        view,
    )
    hint.setStyleSheet("padding: 5px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
 


def _manage_color_formats(dialog):
    """打开颜色格式管理窗口，确定后把结果留在 dialog 上等「应用」。"""
    from .color_format_dialog import ColorFormatDialog

    editor = ColorFormatDialog(dialog.magnifier_color_formats, dialog)
    if editor.exec():
        dialog.magnifier_color_formats = editor.entries()
