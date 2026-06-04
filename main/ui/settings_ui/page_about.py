# -*- coding: utf-8 -*-
"""关于页面"""
import webbrowser

from PySide6.QtWidgets import QWidget, QVBoxLayout, QScrollArea
from PySide6.QtCore import Qt
from qfluentwidgets import (
    SettingCard, FluentIcon,
    HyperlinkButton,
)
from .components import SettingCardGroup
from ui.dialogs import show_text_dialog


def create_about_page(dialog) -> QScrollArea:
    """创建情報页面 - Fluent 风格"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 20, 10, 20)
    layout.setSpacing(16)

    # ── 关于信息 ─────────────────────────────────────────────
    group = SettingCardGroup(dialog.tr("About"), view)

    # 软件信息卡片
    name_card = SettingCard(
        FluentIcon.APPLICATION,
        dialog.tr("Jietuba - Screenshot Tool"),
        "Version: 2026.03.05",
        parent=group,
    )
    group.addSettingCard(name_card)

    # 开发者卡片
    dev_card = SettingCard(
        FluentIcon.PEOPLE,
        dialog.tr("Developer"),
        "RI JYAARU",
        parent=group,
    )
    group.addSettingCard(dev_card)

    license_card = SettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Licenses"),
        "MIT",
        parent=group,
    )
    details_btn = HyperlinkButton(url="", text=dialog.tr("Details"), parent=license_card)
    details_btn.clicked.connect(
        lambda: show_text_dialog(
            dialog,
            dialog.tr("MIT License"),
            dialog.tr("MIT License Text"),
        )
    )
    license_card.hBoxLayout.addWidget(details_btn, 0, Qt.AlignmentFlag.AlignRight)
    license_card.hBoxLayout.addSpacing(16)
    group.addSettingCard(license_card)

    # GitHub 链接卡片
    github_card = SettingCard(
        FluentIcon.GITHUB,
        dialog.tr("Source Code"),
        "github.com/1003129155/jietuba",
        parent=group,
    )
    link_btn = HyperlinkButton(
        url="https://github.com/1003129155/jietuba",
        text=dialog.tr("Open GitHub"),
        parent=github_card,
    )
    github_card.hBoxLayout.addWidget(link_btn, 0, Qt.AlignmentFlag.AlignRight)
    github_card.hBoxLayout.addSpacing(16)
    group.addSettingCard(github_card)

    layout.addWidget(group)
    layout.addStretch(1)
    scroll.setWidget(view)
    return scroll

 