# -*- coding: utf-8 -*-
"""翻译设置页 — Fluent Design

这个文件里不出现任何服务商的名字。每家的凭据、独有开关、提示语、帮助链接，
以及「通用选项里它吃哪些」，全部来自 provider 自己的声明（见
translation/provider.py 的 Field / ProviderMetadata），界面只负责按声明渲染。

以前是每家手写一段表单、dialog.py 再手写一段保存，加一家要在两处各补一次。
补漏了不会报错：azure 漏在表单那侧（选得到、下面空白），baidu 漏在保存那侧
（填了、存不上、一直说未配置）。现在这两种漏法在结构上都不成立了。
"""
from PySide6.QtCore import QPoint, QRect, Qt, QThread, Signal
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QLineEdit, QScrollArea, QFrame, QMenu, QToolTip,
)
from shiboken6 import isValid
from core.ui_scale import dialog_scaled
from ui.fluent_lite.theme import ACCENT, ui_tokens
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, LineEdit,
    PushButton, card_right_margin,
)
from core.ui_theme import get_ui_theme
from .components import (
    SettingCardGroup, WhiteCard, adjust_button_width, apply_theme_text_style,
)
from . import provider_fields

from translation.languages import TRANSLATION_LANGUAGES
from translation.models import TranslationRequest
from translation.provider import ModelField, ToggleField
from translation.service import TranslationService, create_default_translation_service
from translation.worker import TranslationWorker


# 标签列宽。各家字段数和标签长度不同，固定宽度是为了切换服务商时输入框不左右跳。
_LABEL_WIDTH = 135
# 左边距对齐 FSettingCard 的标题位置（图标 + 间距），也对齐分隔线的 margin-left 52
_ROW_LEFT_MARGIN = 56


class _ProviderSection(QWidget):
    """一家翻译服务的设置行容器。

    行为对齐 SettingCardGroup.addSettingCard——行与行之间插 1px 分隔线。区别是
    没有标题、没有外框：外框由包着它的那一个 SettingCardGroup 提供，所以整页
    只剩一个白块。

    分隔线颜色跟随主题，所以要接 theme_changed 自己重刷；SettingCardGroup 也是
    这么做的。
    """

    def __init__(self, parent=None, leading_separator: bool = False):
        super().__init__(parent)
        self._v = QVBoxLayout(self)
        self._v.setContentsMargins(0, 0, 0, 0)
        self._v.setSpacing(0)
        self._cards = []
        self._separators = []
        if leading_separator:
            # 分隔线做成自己的子控件，而不是让外层组去加：这样 section 一隐藏，
            # 线跟着没。交给外层加的话，卡片藏了线还在，会在组底部留一道孤线。
            self._add_separator()
        get_ui_theme().theme_changed.connect(self._apply_separator_theme)

    def _add_separator(self):
        separator = QFrame(self)
        separator.setFixedHeight(dialog_scaled(1))
        self._separators.append(separator)
        self._v.addWidget(separator)

    def addSettingCard(self, card):
        if self._cards:
            self._add_separator()
        card.setParent(self)
        self._v.addWidget(card)
        self._cards.append(card)
        self._apply_separator_theme()

    def _apply_separator_theme(self, _tokens=None):
        color = ui_tokens(self).separator
        for separator in self._separators:
            separator.setStyleSheet(
                f"background: {color}; border: none; margin-left: {dialog_scaled(52)}px;"
            )


def create_translation_page(dialog) -> QWidget:
    """创建翻译设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    page = QWidget()
    page.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    service = create_default_translation_service(dialog.config_manager)
    dialog.translation_registry = service.registry

    # ════ 翻译引擎 ════
    # 引擎下拉和它的凭据是同一件事——选了哪家就配哪家，所以同属一组、一个白块。
    grp_engine = SettingCardGroup(dialog.tr("Translation Engine"), page)

    engine_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Translation Engine"),
        parent=grp_engine,
    )
    dialog.translation_provider_combo = ComboBox(engine_card)
    current_provider = dialog.config_manager.get_translation_provider()
    current_provider_index = 0
    for index, metadata in enumerate(service.registry.available_providers()):
        # 品牌名没有翻译条目，原样返回；只有「自定义」这类名字会变
        dialog.translation_provider_combo.addItem(
            dialog.tr(metadata.display_name), userData=metadata.provider_id
        )
        if metadata.provider_id == current_provider:
            current_provider_index = index
    dialog.translation_provider_combo.setCurrentIndex(current_provider_index)
    engine_card.addControl(dialog.translation_provider_combo)
    grp_engine.addSettingCard(engine_card)

    providers_host = QWidget(grp_engine)
    providers_layout = QVBoxLayout(providers_host)
    providers_layout.setContentsMargins(0, 0, 0, 0)
    providers_layout.setSpacing(0)

    # 每家一个 section，全部按声明渲染。整个 host 作为一张卡加进组里——关键是
    # 只加这一张：若每个 section 各自 addSettingCard，组会在每个前面插一条分隔线，
    # 而同一时刻只有一家可见，另外几条线会孤零零留着。
    dialog.provider_sections = {}
    dialog.provider_field_widgets = {}
    dialog.provider_status_labels = {}
    for metadata in service.registry.available_providers():
        section = _ProviderSection(providers_host)
        for field in tuple(metadata.credentials) + tuple(metadata.options):
            widget = _build_field(dialog, section, field, metadata.provider_id)
            dialog.provider_field_widgets[field.config_key] = widget
        _build_test_row(dialog, section, metadata.provider_id)
        providers_layout.addWidget(section)
        dialog.provider_sections[metadata.provider_id] = section

    grp_engine.addSettingCard(providers_host)
    layout.addWidget(grp_engine)
    dialog.translation_engine_group = grp_engine

    # ════ 翻译选项 ════
    grp_opts = SettingCardGroup(dialog.tr("Translation Options"), page)

    lang_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Target Language"),
        parent=grp_opts,
    )
    dialog.translation_target_combo = ComboBox(lang_card)

    lang_options = [("", dialog.tr("Auto (System)"))]
    lang_options.extend(list(TRANSLATION_LANGUAGES.items()))
    current_lang = dialog.config_manager.get_app_setting(
        "translation_target_lang", ""
    )
    current_index = 0
    for i, (code, name) in enumerate(lang_options):
        dialog.translation_target_combo.addItem(name, userData=code)
        if code == current_lang:
            current_index = i
    dialog.translation_target_combo.setCurrentIndex(current_index)

    lang_card.addControl(dialog.translation_target_combo)
    grp_opts.addSettingCard(lang_card)

    # 不是每家都吃这些参数，所以整体包进一个 section：单独隐藏两张卡的话，
    # 它们前面的分隔线还留在组里，组底部会多出一道孤线。
    # 显不显示由 provider 的 SUPPORTED_REQUEST_OPTIONS 决定，见 _update_provider_groups。
    request_opts_section = _ProviderSection(grp_opts, leading_separator=True)
    dialog.request_options_section = request_opts_section

    split_card = SwitchSettingCard(
        FluentIcon.ALIGNMENT,
        dialog.tr("Ignore Line Breaks"),
        dialog.tr("Merge multi-line text for better translation"),
        parent=request_opts_section,
    )
    split_card.setChecked(dialog.config_manager.get_translation_split_sentences())
    dialog.split_sentences_toggle = split_card
    request_opts_section.addSettingCard(split_card)

    preserve_card = SwitchSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Preserve Formatting"),
        dialog.tr("Keep original text formatting"),
        parent=request_opts_section,
    )
    preserve_card.setChecked(
        dialog.config_manager.get_translation_preserve_formatting()
    )
    dialog.preserve_formatting_toggle = preserve_card
    request_opts_section.addSettingCard(preserve_card)
    grp_opts.addSettingCard(request_opts_section)

    # 键名要和 provider 声明的 SUPPORTED_REQUEST_OPTIONS 对得上
    dialog.request_option_cards = {
        "split_sentences": split_card,
        "preserve_formatting": preserve_card,
    }

    dialog.translation_options_group = grp_opts
    layout.addWidget(grp_opts)

    # 引擎提示行：内容来自当前引擎自己声明的 notice / help_url
    info_label = QLabel(page)
    info_label.setOpenExternalLinks(True)
    info_label.setWordWrap(True)
    info_label.setStyleSheet(
        f"padding: {dialog_scaled(5)}px; font-size: {dialog_scaled(13)}px; color: #999;"
    )
    layout.addWidget(info_label)
    dialog.translation_notice_label = info_label

    dialog.translation_provider_combo.currentIndexChanged.connect(
        lambda _index: _update_provider_groups(dialog)
    )
    _update_provider_groups(dialog)

    layout.addStretch()
    scroll.setWidget(page)
    return scroll


def _build_field(dialog, section, field, provider_id):
    """按 Field 的类型造一行控件，并塞进 section。"""
    if isinstance(field, ToggleField):
        card = SwitchSettingCard(
            _icon_for(field),
            dialog.tr(field.label),
            dialog.tr(field.description) if field.description else "",
            parent=section,
        )
        card.setChecked(
            provider_fields.read_config(dialog.config_manager, field)
        )
        section.addSettingCard(card)
        return card

    card = WhiteCard(section)
    row = QHBoxLayout(card)
    row.setContentsMargins(
        dialog_scaled(_ROW_LEFT_MARGIN), dialog_scaled(12), card_right_margin(), dialog_scaled(12)
    )
    row.setSpacing(dialog_scaled(10))

    title = QLabel(dialog.tr(field.label), card)
    apply_theme_text_style(title, 13, bold=True)
    title.setFixedWidth(dialog_scaled(_LABEL_WIDTH))
    row.addWidget(title)

    edit = LineEdit(card, use_default_style=False)
    edit.setText(provider_fields.read_config(dialog.config_manager, field))
    if field.placeholder:
        edit.setPlaceholderText(dialog.tr(field.placeholder))
    edit.setStyleSheet(dialog._get_input_style())
    if field.secret:
        edit.setEchoMode(QLineEdit.EchoMode.Password)
    row.addWidget(edit, 1)

    # 每个 secret 字段都配一个显示/隐藏。以前这按钮只长在 DeepL 那一行上，
    # 六个 secret 字段只有一个能查看——填错了另外五个，肉眼没法核对。
    if field.secret:
        button = PushButton(dialog.tr("Show"), card)
        button.setFixedHeight(dialog_scaled(32))
        adjust_button_width(button, min_width=60)
        button.clicked.connect(
            lambda _checked=False, e=edit, b=button: _toggle_secret(dialog, e, b)
        )
        row.addWidget(button)

    if isinstance(field, ModelField):
        button = PushButton(dialog.tr("Fetch Models"), card)
        button.setFixedHeight(dialog_scaled(32))
        adjust_button_width(button, min_width=60)
        button.clicked.connect(
            lambda _checked=False, e=edit, b=button:
                _fetch_models(dialog, provider_id, e, b)
        )
        row.addWidget(button)

    card.setFixedHeight(dialog_scaled(58))
    section.addSettingCard(card)
    return edit


def _build_test_row(dialog, section, provider_id):
    """每家最后一行：用表单里的当前值（含未保存的）发一句测试翻译。"""
    card = WhiteCard(section)
    row = QHBoxLayout(card)
    row.setContentsMargins(
        dialog_scaled(_ROW_LEFT_MARGIN), dialog_scaled(12), card_right_margin(), dialog_scaled(12)
    )
    row.setSpacing(dialog_scaled(10))

    status = QLabel(card)
    status.setWordWrap(True)
    status.setTextInteractionFlags(status.textInteractionFlags() | Qt.TextInteractionFlag.TextSelectableByMouse)
    apply_theme_text_style(status, 13, caption=True)
    row.addWidget(status, 1)
    dialog.provider_status_labels[provider_id] = status

    button = PushButton(dialog.tr("Test Connection"), card)
    button.setFixedHeight(dialog_scaled(32))
    adjust_button_width(button, min_width=60)
    button.clicked.connect(
        lambda _checked=False, b=button: _test_connection(dialog, provider_id, b)
    )
    row.addWidget(button)

    card.setMinimumHeight(dialog_scaled(58))
    section.addSettingCard(card)


# 后台请求的线程。不挂在设置窗口上：窗口关掉时线程可能还在等网络，
# 父对象一析构就会连带销毁正在运行的 QThread。
_running_threads = set()


def _keep_until_finished(thread):
    _running_threads.add(thread)

    def release():
        _running_threads.discard(thread)
        thread.deleteLater()

    thread.finished.connect(release)
    thread.start()


class _ModelListThread(QThread):
    finished_signal = Signal(bool, object)  # (成功, 模型列表 / 错误信息)

    def __init__(self, provider):
        super().__init__()
        self._provider = provider

    def run(self):
        try:
            self.finished_signal.emit(True, self._provider.list_models())
        except Exception as exc:
            self.finished_signal.emit(False, str(exc))


def _pending_service(dialog, provider_id):
    fields = provider_fields.provider_fields(dialog.translation_registry, provider_id)
    config = provider_fields.PendingConfig(
        dialog.config_manager, fields, dialog.provider_field_widgets
    )
    return TranslationService(dialog.translation_registry, config)


def _show_status(dialog, provider_id, text):
    label = dialog.provider_status_labels.get(provider_id)
    if label is not None and isValid(label):
        label.setText(text)


def _test_connection(dialog, provider_id, button):
    target = ""
    combo = getattr(dialog, "translation_target_combo", None)
    if combo is not None:
        target = combo.currentData() or ""
    request = TranslationRequest("Hello, world!", target or "ZH")
    worker = TranslationWorker(
        _pending_service(dialog, provider_id), request, provider_id=provider_id
    )

    button.setEnabled(False)
    _show_status(dialog, provider_id, dialog.tr("Testing..."))

    def done(result):
        if not isValid(button):
            return
        button.setEnabled(True)
        if result.success:
            _show_status(dialog, provider_id, "✓ " + result.translated_text)
        else:
            _show_status(dialog, provider_id, "✗ " + result.error_message)

    worker.finished_signal.connect(done)
    _keep_until_finished(worker)


def _fetch_models(dialog, provider_id, edit, button):
    try:
        provider = _pending_service(dialog, provider_id).provider(provider_id)
    except ValueError as exc:
        _show_status(dialog, provider_id, "✗ " + str(exc))
        return
    thread = _ModelListThread(provider)

    button.setEnabled(False)
    _show_status(dialog, provider_id, dialog.tr("Fetching models..."))

    def done(ok, value):
        if not isValid(button):
            return
        button.setEnabled(True)
        if not ok or not value:
            text = "✗ " + value if not ok else dialog.tr("No models returned")
            _show_status(dialog, provider_id, text)
            _show_near(button, text)
            return
        _show_status(dialog, provider_id, "")
        menu = QMenu(edit)
        for model in value:
            menu.addAction(model).triggered.connect(
                lambda _checked=False, m=model: edit.setText(m)
            )
        menu.exec(edit.mapToGlobal(QPoint(0, edit.height())))

    thread.finished_signal.connect(done)
    _keep_until_finished(thread)


def _show_near(button, text):
    """状态行在这一家表单的最底下，离模型那一行隔着好几行高级选项，
    失败时在按钮旁边再提示一次。

    不传 button 作为所属控件：传了的话 Qt 会把按钮的样式表套到提示框上，
    不再用全局主题里的 QToolTip 配色，结果是黑底深色字。
    """
    QToolTip.showText(
        button.mapToGlobal(QPoint(0, button.height())), text, None, QRect(), 8000
    )


def _icon_for(field):
    """ToggleField 声明的图标名解析成 FluentIcon，认不出就用一个中性的。"""
    return getattr(FluentIcon, field.icon_name, None) or FluentIcon.SETTING


def _toggle_secret(dialog, edit, button):
    if edit.echoMode() == QLineEdit.EchoMode.Password:
        edit.setEchoMode(QLineEdit.EchoMode.Normal)
        button.setText(dialog.tr("Hide"))
    else:
        edit.setEchoMode(QLineEdit.EchoMode.Password)
        button.setText(dialog.tr("Show"))
    adjust_button_width(button, min_width=60)


def _update_provider_groups(dialog) -> None:
    """只展开当前引擎的设置，并按它声明的能力决定通用选项显不显示。

    这里刻意不认服务商名字：未知/空 provider 时全部隐藏，而不是留着上一家的
    输入框——注册表是动态的，出现没见过的值是正常情况。
    """
    provider_id = dialog.translation_provider_combo.currentData()

    for pid, section in getattr(dialog, "provider_sections", {}).items():
        section.setVisible(pid == provider_id)

    metadata = None
    registry = getattr(dialog, "translation_registry", None)
    if registry is not None and provider_id:
        try:
            metadata = registry.metadata(provider_id)
        except ValueError:
            metadata = None

    supported = metadata.supported_request_options if metadata else frozenset()
    for key, card in getattr(dialog, "request_option_cards", {}).items():
        card.setVisible(key in supported)
    section = getattr(dialog, "request_options_section", None)
    if section is not None:
        section.setVisible(bool(supported))

    label = getattr(dialog, "translation_notice_label", None)
    if label is not None:
        label.setText(_notice_html(dialog, metadata))
        label.setVisible(bool(metadata and (metadata.notice or metadata.help_url)))

    for attr in ("translation_engine_group", "translation_options_group"):
        group = getattr(dialog, attr, None)
        if group is not None:
            group.refreshHeight()


def _notice_html(dialog, metadata) -> str:
    if metadata is None:
        return ""
    parts = []
    if metadata.notice:
        parts.append(dialog.tr(metadata.notice))
    if metadata.help_url:
        label = dialog.tr(metadata.help_label) if metadata.help_label else ""
        parts.append(
            f'<a href="{metadata.help_url}" style="color:{ACCENT};">'
            f'{label or metadata.help_url}</a>'
        )
    return ("💡 " + " ".join(parts)) if parts else ""
