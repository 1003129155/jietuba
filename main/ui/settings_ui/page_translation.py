# -*- coding: utf-8 -*-
"""翻译设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QLineEdit, QScrollArea, QFrame,
)
from PySide6.QtCore import Qt
from ui.fluent_lite.theme import ACCENT, ui_tokens
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, LineEdit,
    PushButton,
)
from core.ui_theme import get_ui_theme
from .components import SettingCardGroup, WhiteCard, adjust_button_width, apply_theme_text_style

from translation.languages import TRANSLATION_LANGUAGES
from translation.service import create_default_translation_service


class _ProviderSection(QWidget):
    """一家翻译服务的凭证行容器。

    行为对齐 SettingCardGroup.addSettingCard——行与行之间插 1px 分隔线，于是
    _add_text_setting 不用改就能往这里加行。区别是没有标题、没有外框：外框由
    包着它的那一个 SettingCardGroup 提供，所以整页只剩一个白块。

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
        separator.setFixedHeight(1)
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
                f"background: {color}; border: none; margin-left: 52px;"
            )


def create_translation_page(dialog) -> QWidget:
    """创建翻译设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    page = QWidget()
    page.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ════ 翻译引擎 ════
    # 引擎下拉和它的凭证是同一件事——选了哪家就配哪家。原先拆成两个
    # SettingCardGroup，渲染出来是两个各带标题的白块，中间隔一道，读起来像两项
    # 互不相干的设置；Google 那种只有一个字段的，更是一个大标题配一行。
    #
    # 现在合成一组：五家凭证各自装进 _ProviderSection，再由 providers_host 整个
    # 作为一张卡加进来。关键是只加这一张——若五个 section 各自 addSettingCard，
    # 组会在每个前面插一条分隔线，而同一时刻只有一家可见，另外四条线会孤零零留着。
    grp_engine = SettingCardGroup(dialog.tr("Translation Engine"), page)

    engine_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Translation Engine"),
        parent=grp_engine,
    )
    dialog.translation_provider_combo = ComboBox(engine_card)
    dialog.translation_provider_combo.setFixedWidth(180)
    service = create_default_translation_service(dialog.config_manager)
    current_provider = dialog.config_manager.get_translation_provider()
    current_provider_index = 0
    for index, metadata in enumerate(service.registry.available_providers()):
        dialog.translation_provider_combo.addItem(
            metadata.display_name, userData=metadata.provider_id
        )
        if metadata.provider_id == current_provider:
            current_provider_index = index
    dialog.translation_provider_combo.setCurrentIndex(current_provider_index)
    engine_card.hBoxLayout.addWidget(
        dialog.translation_provider_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    engine_card.hBoxLayout.addSpacing(16)
    grp_engine.addSettingCard(engine_card)

    providers_host = QWidget(grp_engine)
    providers_layout = QVBoxLayout(providers_host)
    providers_layout.setContentsMargins(0, 0, 0, 0)
    providers_layout.setSpacing(0)

    # ── DeepL ──
    sec_deepl = _ProviderSection(providers_host)

    key_card = WhiteCard(sec_deepl)
    key_h = QHBoxLayout(key_card)
    # 与 _add_text_setting 同一左边距，各家切换时标签不会左右跳
    key_h.setContentsMargins(56, 12, 20, 12)
    key_h.setSpacing(10)
    key_lbl = QLabel(dialog.tr("DeepL API Key"), key_card)
    apply_theme_text_style(key_lbl, 14)
    # 与 _add_text_setting 的标签同宽，各家切换时输入框不会左右跳
    key_lbl.setFixedWidth(135)
    key_h.addWidget(key_lbl)
    dialog.deepl_api_key_input = LineEdit(key_card, use_default_style=False)
    dialog.deepl_api_key_input.setPlaceholderText(
        "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx:fx"
    )
    dialog.deepl_api_key_input.setText(dialog.config_manager.get_deepl_api_key())
    dialog.deepl_api_key_input.setEchoMode(LineEdit.EchoMode.Password)
    dialog.deepl_api_key_input.setStyleSheet(dialog._get_input_style())
    key_h.addWidget(dialog.deepl_api_key_input, 1)
    dialog.show_api_key_btn = PushButton(dialog.tr("Show"), key_card)
    dialog.show_api_key_btn.setFixedHeight(32)
    adjust_button_width(dialog.show_api_key_btn, min_width=60)
    dialog.show_api_key_btn.clicked.connect(
        lambda: _toggle_api_key_visibility(dialog)
    )
    key_h.addWidget(dialog.show_api_key_btn)
    key_card.setFixedHeight(58)
    sec_deepl.addSettingCard(key_card)

    pro_card = SwitchSettingCard(
        FluentIcon.CERTIFICATE,
        dialog.tr("Use DeepL Pro API"),
        dialog.tr("Enable if you have a paid DeepL subscription"),
        parent=sec_deepl,
    )
    pro_card.setChecked(dialog.config_manager.get_deepl_use_pro())
    dialog.deepl_pro_toggle = pro_card
    sec_deepl.addSettingCard(pro_card)
    providers_layout.addWidget(sec_deepl)

    # ── Amazon Translate ──
    sec_amazon = _ProviderSection(providers_host)
    dialog.amazon_translate_region_input = _add_text_setting(
        dialog, sec_amazon, dialog.tr("AWS Region"),
        dialog.config_manager.get_amazon_translate_region(), "us-west-2",
    )
    dialog.amazon_translate_access_key_input = _add_text_setting(
        dialog, sec_amazon, dialog.tr("Access Key ID"),
        dialog.config_manager.get_amazon_translate_access_key_id(), "AKIA...",
    )
    dialog.amazon_translate_secret_key_input = _add_text_setting(
        dialog, sec_amazon, dialog.tr("Secret Access Key"),
        dialog.config_manager.get_amazon_translate_secret_access_key(),
        dialog.tr("Required"), password=True,
    )
    dialog.amazon_translate_session_token_input = _add_text_setting(
        dialog, sec_amazon, dialog.tr("Session Token"),
        dialog.config_manager.get_amazon_translate_session_token(),
        dialog.tr("Optional, for temporary credentials"), password=True,
    )
    providers_layout.addWidget(sec_amazon)

    # ── Google Cloud Translation ──
    sec_google = _ProviderSection(providers_host)
    dialog.google_translate_api_key_input = _add_text_setting(
        dialog, sec_google, dialog.tr("Google API Key"),
        dialog.config_manager.get_google_translate_api_key(), "AIza...",
        password=True,
    )
    providers_layout.addWidget(sec_google)

    # ── Azure Translator ──
    sec_azure = _ProviderSection(providers_host)
    dialog.azure_translate_api_key_input = _add_text_setting(
        dialog, sec_azure, dialog.tr("Azure API Key"),
        dialog.config_manager.get_azure_translate_api_key(),
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", password=True,
    )
    dialog.azure_translate_region_input = _add_text_setting(
        dialog, sec_azure, dialog.tr("Azure Region"),
        dialog.config_manager.get_azure_translate_region(), "eastasia",
    )
    dialog.azure_translate_endpoint_input = _add_text_setting(
        dialog, sec_azure, dialog.tr("Azure Endpoint"),
        dialog.config_manager.get_azure_translate_endpoint(),
        dialog.tr("Optional, use default if empty"),
    )
    providers_layout.addWidget(sec_azure)

    # ── Baidu Translate ──
    sec_baidu = _ProviderSection(providers_host)
    dialog.baidu_translate_appid_input = _add_text_setting(
        dialog, sec_baidu, dialog.tr("Baidu APPID"),
        dialog.config_manager.get_baidu_translate_appid(), "APPID",
    )
    # 标签是「密钥」不是「API Key」：控制台上与 APPID 成对的那一栏就叫密钥，
    # 而百度另有一个叫 API Key 的凭据（大模型单独申请的）。叫错名字用户会填错，
    # 而填错的报错是 54001 invalid token，界面上看不出该换哪个值。
    dialog.baidu_translate_secret_key_input = _add_text_setting(
        dialog, sec_baidu, dialog.tr("Baidu Secret Key"),
        dialog.config_manager.get_baidu_translate_secret_key(),
        dialog.tr("Paired with APPID in the Baidu console"),
        password=True,
    )
    providers_layout.addWidget(sec_baidu)

    grp_engine.addSettingCard(providers_host)
    layout.addWidget(grp_engine)

    # 切换服务商会改变这两个组里可见卡片的行数，_update_provider_groups
    # 收尾时要让它们重算高度
    dialog.translation_engine_group = grp_engine
    dialog.deepl_settings_group = sec_deepl
    dialog.amazon_translate_settings_group = sec_amazon
    dialog.google_translate_settings_group = sec_google
    dialog.azure_translate_settings_group = sec_azure
    dialog.baidu_translate_settings_group = sec_baidu
    dialog.translation_provider_combo.currentIndexChanged.connect(
        lambda _index: _update_provider_groups(dialog)
    )
    _update_provider_groups(dialog)

    # ════ 翻译选项 ════
    grp_opts = SettingCardGroup(dialog.tr("Translation Options"), page)

    # 目标语言
    lang_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Target Language"),
        parent=grp_opts,
    )
    dialog.translation_target_combo = ComboBox(lang_card)
    dialog.translation_target_combo.setFixedWidth(180)

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

    lang_card.hBoxLayout.addWidget(
        dialog.translation_target_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    lang_card.hBoxLayout.addSpacing(16)
    grp_opts.addSettingCard(lang_card)

    # 忽略换行、保留格式都是 DeepL 专属。包进 section 是为了那条分隔线——
    # 单独隐藏两张卡的话，它们前面的分隔线还留在组里，组底部会多出一道孤线。
    sec_deepl_opts = _ProviderSection(grp_opts, leading_separator=True)
    dialog.deepl_options_section = sec_deepl_opts

    split_card = SwitchSettingCard(
        FluentIcon.ALIGNMENT,
        dialog.tr("Ignore Line Breaks"),
        dialog.tr("Merge multi-line text for better translation"),
        parent=sec_deepl_opts,
    )
    split_card.setChecked(dialog.config_manager.get_translation_split_sentences())
    dialog.split_sentences_toggle = split_card
    sec_deepl_opts.addSettingCard(split_card)

    # 保留格式
    preserve_card = SwitchSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Preserve Formatting"),
        dialog.tr("Keep original text formatting"),
        parent=sec_deepl_opts,
    )
    preserve_card.setChecked(
        dialog.config_manager.get_translation_preserve_formatting()
    )
    dialog.preserve_formatting_toggle = preserve_card
    sec_deepl_opts.addSettingCard(preserve_card)
    grp_opts.addSettingCard(sec_deepl_opts)

    dialog.translation_options_group = grp_opts
    layout.addWidget(grp_opts)

    # 提示
    info_label = QLabel(
        "💡 " + dialog.tr("DeepL free tier: 500,000 chars/month. Get API key at")
        + f' <a href="https://www.deepl.com/pro-api" style="color:{ACCENT};">deepl.com/pro-api</a>',
        page,
    )
    info_label.setOpenExternalLinks(True)
    info_label.setWordWrap(True)
    info_label.setStyleSheet("padding: 5px; font-size: 12px; color: #999;")
    layout.addWidget(info_label)
    dialog.deepl_translation_info_label = info_label
    _update_provider_groups(dialog)

    layout.addStretch()
    scroll.setWidget(page)
    return scroll


def _toggle_api_key_visibility(dialog):
    """切换 API 密钥显示/隐藏"""
    if dialog.deepl_api_key_input.echoMode() == QLineEdit.EchoMode.Password:
        dialog.deepl_api_key_input.setEchoMode(QLineEdit.EchoMode.Normal)
        dialog.show_api_key_btn.setText(dialog.tr("Hide"))
        adjust_button_width(dialog.show_api_key_btn, min_width=60)
    else:
        dialog.deepl_api_key_input.setEchoMode(QLineEdit.EchoMode.Password)
        dialog.show_api_key_btn.setText(dialog.tr("Show"))
        adjust_button_width(dialog.show_api_key_btn, min_width=60)


def _add_text_setting(
    dialog,
    group,
    label: str,
    value: str,
    placeholder: str,
    *,
    password: bool = False,
):
    card = WhiteCard(group)
    row = QHBoxLayout(card)
    # 左边距 56 对齐 FSettingCard 的标题位置（图标 + 间距），也对齐分隔线的
    # margin-left 52。用 20 的话标签会比上面的「翻译引擎」往左突出一截。
    row.setContentsMargins(56, 12, 20, 12)
    row.setSpacing(10)
    title = QLabel(label, card)
    apply_theme_text_style(title, 14)
    title.setFixedWidth(135)
    row.addWidget(title)
    edit = LineEdit(card, use_default_style=False)
    edit.setText(value or "")
    edit.setPlaceholderText(placeholder)
    if password:
        edit.setEchoMode(QLineEdit.EchoMode.Password)
    edit.setStyleSheet(dialog._get_input_style())
    row.addWidget(edit, 1)
    card.setFixedHeight(58)
    group.addSettingCard(card)
    return edit


def _update_provider_groups(dialog) -> None:
    provider_id = dialog.translation_provider_combo.currentData()
    dialog.deepl_settings_group.setVisible(provider_id == "deepl")
    dialog.amazon_translate_settings_group.setVisible(
        provider_id == "amazon"
    )
    dialog.google_translate_settings_group.setVisible(
        provider_id == "google"
    )
    dialog.azure_translate_settings_group.setVisible(
        provider_id == "azure"
    )
    dialog.baidu_translate_settings_group.setVisible(
        provider_id == "baidu"
    )
    if hasattr(dialog, "deepl_translation_info_label"):
        dialog.deepl_translation_info_label.setVisible(
            provider_id == "deepl"
        )
    if hasattr(dialog, "deepl_options_section"):
        dialog.deepl_options_section.setVisible(provider_id == "deepl")
    if hasattr(dialog, "split_sentences_toggle"):
        dialog.split_sentences_toggle.setVisible(
            provider_id == "deepl"
        )
    if hasattr(dialog, "preserve_formatting_toggle"):
        dialog.preserve_formatting_toggle.setVisible(
            provider_id == "deepl"
        )
    for attr in ("translation_engine_group", "translation_options_group"):
        group = getattr(dialog, attr, None)
        if group is not None:
            group.refreshHeight()
