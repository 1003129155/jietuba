# -*- coding: utf-8 -*-
"""截图设置页 — Fluent Design"""
from PySide6.QtWidgets import QWidget, QVBoxLayout
from core.ui_scale import dialog_scaled
from ui.fluent_lite import (
    SwitchSettingCard, SubSwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel, PushButton,
)
from settings import color_formats
from settings.tool_settings import (
    CAPTURE_ENGINES, OCR_RESULT_POSITIONS, SMART_SELECTION_MODES,
)
from .components import SettingCardGroup, page_scroll_area
from core.ui_theme import set_own_style


def create_capture_page(dialog) -> QWidget:
    """截图設定 ─ 截图引擎 + 智能选区 + 保存设置 + 放大镜 + OCR"""
    scroll = page_scroll_area(dialog)
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    # ── 截图引擎 ──────────────────────────────────────
    grp_engine = SettingCardGroup(dialog.tr("Screenshot"), view, tone="capture")
    engine_card = FSettingCard(
        FluentIcon.CAMERA,
        dialog.tr("Capture Engine"),
        dialog.tr("Auto uses the HDR engine when a display has HDR turned on."),
        parent=grp_engine,
    )
    dialog.capture_engine_combo = ComboBox(engine_card)
    for label, engine in ((dialog.tr("Auto (Recommended)"), "auto"),
                          (dialog.tr("Standard"), "mss"),
                          ("HDR", "hdr")):
        dialog.capture_engine_combo.addItem(label, userData=engine)
    dialog.capture_engine_combo.setCurrentIndex(
        CAPTURE_ENGINES.index(dialog.config_manager.get_capture_engine())
    )
    engine_card.addControl(dialog.capture_engine_combo)
    grp_engine.addSettingCard(engine_card)
    layout.addWidget(grp_engine)

    # ── 智能选区 ──────────────────────────────────────
    grp_smart = SettingCardGroup(dialog.tr("Smart Selection"), view, tone="capture")

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
    grp_save = SettingCardGroup(dialog.tr("Save Settings"), view, tone="capture")

    # 自动保存按时机各设一个开关，挂在这张标题卡片下面；判断在 deliver_screenshot 里
    grp_save.addSettingCard(FSettingCard(
        FluentIcon.SAVE,
        dialog.tr("Auto-save"),
        dialog.tr("Screenshots are saved to the save folder automatically when the actions below finish."),
        parent=grp_save,
    ))

    save_card = SubSwitchSettingCard(
        dialog.tr("When a Screenshot Is Done"),
        dialog.tr("Saves when a screenshot is finished, including confirming a screenshot, long screenshots, "
                  "global mouse shortcut screenshots, and more."),
        parent=grp_save,
    )
    save_card.setChecked(dialog.config_manager.get_screenshot_save_enabled())
    dialog.save_toggle = save_card
    grp_save.addSettingCard(save_card)

    pin_save_card = SubSwitchSettingCard(
        dialog.tr("When a Pinned Image Is Created"),
        dialog.tr("Saves when a selected region becomes a pinned image, including regular pins, "
                  "global mouse shortcut pins, long screenshot pins, and more."),
        parent=grp_save,
    )
    pin_save_card.setChecked(dialog.config_manager.get_screenshot_pin_save_enabled())
    dialog.screenshot_pin_save_toggle = pin_save_card
    grp_save.addSettingCard(pin_save_card)

    # 写入文件路径到剪贴板（CF_HDROP）。这次截图自动保存了才有路径可写，判断在
    # deliver_screenshot 里；不在界面上做联动禁用，开关各自独立存储，互不覆盖。
    file_ref_card = SwitchSettingCard(
        FluentIcon.COMMAND_PROMPT,
        dialog.tr("Write File Path to Clipboard"),
        dialog.tr(
            "Lets tools that only recognize a file path (e.g. some terminal apps) "
            "paste the screenshot too. Only takes effect when the screenshot is auto-saved."
        ),
        parent=grp_save,
    )
    file_ref_card.setChecked(dialog.config_manager.get_clipboard_file_reference_enabled())
    dialog.clipboard_file_reference_toggle = file_ref_card
    grp_save.addSettingCard(file_ref_card)

    original_card = SwitchSettingCard(
        FluentIcon.HISTORY,
        dialog.tr("Also Save the Original"),
        dialog.tr(
            "When a screenshot with annotations (including mosaic) or a watermark is saved to the save folder, "
            "a copy without them is saved too, with _original at the end of its file name."
        ),
        parent=grp_save,
    )
    original_card.setChecked(dialog.config_manager.get_screenshot_save_original_enabled())
    dialog.screenshot_save_original_toggle = original_card
    grp_save.addSettingCard(original_card)

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

    # ── 水印 ──────────────────────────────────────────
    # 文字和样式都在截图工具栏的水印面板里改，这里只管要不要沿用上次的水印
    grp_watermark = SettingCardGroup(dialog.tr("Watermark"), view, tone="capture")

    watermark_auto_card = SwitchSettingCard(
        FluentIcon.SPARKLE,
        dialog.tr("Add the Last Watermark Automatically"),
        dialog.tr("If the last screenshot ended with a watermark, the next one gets the same watermark "
                  "with the time updated; otherwise none is added. Screenshots taken with mouse shortcuts "
                  "follow the same rule."),
        parent=grp_watermark,
    )
    watermark_auto_card.setChecked(dialog.config_manager.get_screenshot_watermark_auto())
    dialog.watermark_auto_toggle = watermark_auto_card
    grp_watermark.addSettingCard(watermark_auto_card)

    layout.addWidget(grp_watermark)

    # ── 放大镜 ────────────────────────────────────────
    grp_magnifier = SettingCardGroup(dialog.tr("Magnifier"), view, tone="capture")

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

    hint_card = SwitchSettingCard(
        FluentIcon.INFO,
        dialog.tr("Shortcut Hint"),
        dialog.tr("Show the switch-format and copy-color shortcuts under the readings. "
                  "Off makes the magnifier shorter."),
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
        dialog.tr("Which formats Shift cycles through on the magnifier, for C to copy."),
        parent=grp_magnifier,
    )
    dialog.magnifier_color_formats = color_formats.load(dialog.config_manager)
    manage_btn = PushButton(dialog.tr("Manage Color Formats"), fmt_card)
    manage_btn.clicked.connect(lambda: _manage_color_formats(dialog))
    dialog.magnifier_color_formats_btn = manage_btn
    fmt_card.addControl(manage_btn)
    grp_magnifier.addSettingCard(fmt_card)

    layout.addWidget(grp_magnifier)

    # ── OCR ───────────────────────────────────────────
    try:
        from ocr import get_available_engines, get_ppocr_status
        available_engines = get_available_engines()
        ppocr_status = get_ppocr_status()
    except Exception:
        available_engines = []
        ppocr_status = "missing_engine"
    ocr_available = bool(available_engines)
    dialog.ocr_has_oneocr = "oneocr" in available_engines
    dialog.ocr_has_ppocr_engine = ppocr_status != "missing_engine"

    grp_ocr = SettingCardGroup(dialog.tr("OCR Settings"), view, tone="capture")
    ocr_engine_card = FSettingCard(
        FluentIcon.FONT,
        dialog.tr("OCR Engine"),
        dialog.tr("Switch OCR engines here."),
        parent=grp_ocr,
    )
    dialog.ocr_engine_combo = ComboBox(ocr_engine_card)
    fill_ocr_engines(dialog, dialog.config_manager.get_ocr_engine(), dialog.config_manager.get_ppocr_model())
    dialog.ocr_engine_combo.activated.connect(lambda _: _on_ocr_engine_activated(dialog))
    ocr_engine_card.addControl(dialog.ocr_engine_combo)
    grp_ocr.addSettingCard(ocr_engine_card)

    # 管理模型只在带 ppocr_rust 的构建里出现；下拉框最后一项「下载更多模型…」打开的是同一个窗口
    if dialog.ocr_has_ppocr_engine:
        manage_card = FSettingCard(
            FluentIcon.DOWNLOAD,
            dialog.tr("Manage PP-OCR Models"),
            dialog.tr("Download models for other languages or speeds, or add your own."),
            parent=grp_ocr,
        )
        manage_models_btn = PushButton(dialog.tr("Manage Models"), manage_card)
        manage_models_btn.clicked.connect(lambda: _open_model_manager(dialog))
        manage_card.addControl(manage_models_btn)
        grp_ocr.addSettingCard(manage_card)

    ocr_position_card = FSettingCard(
        FluentIcon.LAYOUT,
        dialog.tr("OCR Result Window Position"),
        dialog.tr("Where the text recognition result window appears."),
        parent=grp_ocr,
    )
    dialog.ocr_result_position_combo = ComboBox(ocr_position_card)
    # 文案顺序要和 OCR_RESULT_POSITIONS 一致
    position_labels = (
        dialog.tr("Capture display center"),
        dialog.tr("Primary display center"),
        dialog.tr("Beside capture area"),
        dialog.tr("Near cursor"),
        dialog.tr("Last position"),
    )
    for label, position in zip(position_labels, OCR_RESULT_POSITIONS):
        dialog.ocr_result_position_combo.addItem(label, userData=position)
    dialog.ocr_result_position_combo.setCurrentIndex(
        OCR_RESULT_POSITIONS.index(dialog.config_manager.get_ocr_result_position())
    )
    ocr_position_card.addControl(dialog.ocr_result_position_combo)
    grp_ocr.addSettingCard(ocr_position_card)
    if not ocr_available:
        grp_ocr.addSettingCard(FSettingCard(
            FluentIcon.INFO,
            dialog.tr("OCR unavailable: install the Snipping Tool from the Microsoft Store, "
                      "or download the full version"),
            parent=grp_ocr,
        ))
    layout.addWidget(grp_ocr)

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
 


PPOCR_PREFIX = "ppocr_rust:"


def ocr_choice(data):
    """下拉框的一项 → (引擎, PP-OCR 模型 id)；不是某个 PP-OCR 模型时模型为 None。"""
    if data and data.startswith(PPOCR_PREFIX):
        return "ppocr_rust", data[len(PPOCR_PREFIX):]
    return data, None


MANAGE_MODELS = "manage_models"


def fill_ocr_engines(dialog, engine: str, model_id: str):
    """OCR 下拉框只列现在能选的：自动、截图工具 OCR（本机有时）、已装的 PP-OCR 模型（清单里的按清单顺序，
    手动添加的在后），最后是「下载更多模型…」，选它打开模型管理窗口。

    选中的模型被删了时仍列出它并标明未安装，不悄悄换成别的；只有这一项会是灰的。
    """
    combo = dialog.ocr_engine_combo
    items = []  # (显示名, 值, 能否选)
    if dialog.ocr_has_oneocr:
        items.append((dialog.tr("Snipping Tool OCR"), "oneocr", True))
    if dialog.ocr_has_ppocr_engine:
        from ocr import ppocr_models
        try:
            installed, catalog = ppocr_models.installed(), ppocr_models.catalog()["models"]
        except Exception:
            installed, catalog = [], []
        order = {entry["id"]: index for index, entry in enumerate(catalog)}
        installed.sort(key=lambda m: order.get(m.get("catalog_id"), len(order)))
        items += [(m["name"], PPOCR_PREFIX + m["id"], True) for m in installed]
    items.insert(0, (dialog.tr("Auto (Recommended)"), "auto", bool(items)))
    selected = PPOCR_PREFIX + model_id if engine == "ppocr_rust" else engine
    if selected not in [data for _, data, _ in items]:
        name = (model_id or "PP-OCRv6 small") if engine == "ppocr_rust" else dialog.tr("Snipping Tool OCR")
        items.append((dialog.tr("%1 (not installed)").replace("%1", name), selected, False))
    combo.blockSignals(True)
    combo.clear()
    for label, data, enabled in items:
        combo.addItem(label, userData=data)
        combo.model().item(combo.count() - 1).setEnabled(enabled)
    if dialog.ocr_has_ppocr_engine:
        combo.insertSeparator(combo.count())
        combo.addItem(dialog.tr("Download more models..."), userData=MANAGE_MODELS)
    combo.setCurrentIndex(combo.findData(selected))
    combo.blockSignals(False)
    dialog.ocr_engine_selected = selected


def _on_ocr_engine_activated(dialog):
    """选了「下载更多模型…」就打开管理窗口。"""
    data = dialog.ocr_engine_combo.currentData()
    if data == MANAGE_MODELS:
        _open_model_manager(dialog)
    else:
        dialog.ocr_engine_selected = data


def _open_model_manager(dialog):
    """打开模型管理窗口；模型增删立即生效，关窗后重填 OCR 列表，选中项回到原来那一项。"""
    from ui.dialogs import exec_dialog
    from .ppocr_model_dialog import PpocrModelDialog

    window = PpocrModelDialog(dialog)
    window.model_removed.connect(lambda model_id: _on_model_removed(dialog, window, model_id))
    exec_dialog(window)
    engine, model_id = ocr_choice(dialog.ocr_engine_selected)
    fill_ocr_engines(dialog, engine, model_id if model_id is not None else dialog.config_manager.get_ppocr_model())


def _on_model_removed(dialog, window, model_id):
    """删掉的正是在用的模型：设置当场改成「自动」，不等「应用」，免得留着一个指向已删模型的设置。
    只是下拉框里选了还没应用的，就把下拉框改回「自动」。"""
    config = dialog.config_manager
    in_use = config.get_ocr_engine() == "ppocr_rust" and config.get_ppocr_model() == model_id
    if in_use:
        config.set_ocr_engine("auto")
        from ocr import set_ocr_engine
        set_ocr_engine("auto")
        snapshot = getattr(dialog, "_settings_snapshot", None)
        if snapshot is not None and "ocr_engine_combo" in snapshot:
            snapshot["ocr_engine_combo"] = "auto"
    if in_use or dialog.ocr_engine_selected == PPOCR_PREFIX + model_id:
        dialog.ocr_engine_selected = "auto"
        window.show_status(dialog.tr("The deleted model was selected for OCR, so OCR now uses Auto."))


def _manage_color_formats(dialog):
    """打开颜色格式管理窗口，确定后把结果留在 dialog 上等「应用」。"""
    from ui.dialogs import exec_dialog
    from .color_format_dialog import ColorFormatDialog

    editor = ColorFormatDialog(dialog.magnifier_color_formats, dialog)
    if exec_dialog(editor):
        dialog.magnifier_color_formats = editor.entries()
