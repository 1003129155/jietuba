# -*- coding: utf-8 -*-
"""关于页面"""

from PySide6.QtWidgets import QWidget, QVBoxLayout, QScrollArea
from PySide6.QtCore import Qt
from core.ui_scale import dialog_scaled
from ui.fluent_lite import (
    SettingCard, FluentIcon,
    HyperlinkButton, PushButton,
)
from .components import SettingCardGroup
from ui.dialogs import (
    show_info_dialog,
    show_text_dialog,
    show_update_dialog,
    show_warning_dialog,
)
from core import T, log_warning
from core.constants import PROJECT_GITHUB_URL
from core.update_checker import GitHubReleaseChecker, ReleaseInfo, is_newer_version
from main_app import APP_VERSION


def create_about_page(dialog) -> QScrollArea:
    """创建情報页面 - Fluent 风格"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, dialog_scaled(20), dialog_scaled(10), dialog_scaled(20))
    layout.setSpacing(dialog_scaled(16))

    # ── 关于信息 ─────────────────────────────────────────────
    group = SettingCardGroup(dialog.tr("About"), view)

    # 软件信息卡片
    name_card = SettingCard(
        FluentIcon.APPLICATION,
        dialog.tr("Version"),
        APP_VERSION,
        parent=group,
    )
    check_update_btn = PushButton(dialog.tr("Check for Updates"), name_card)
    check_update_btn.setObjectName("checkUpdateButton")
    name_card.hBoxLayout.addWidget(
        check_update_btn, 0, Qt.AlignmentFlag.AlignRight
    )
    name_card.hBoxLayout.addSpacing(dialog_scaled(16))
    group.addSettingCard(name_card)

    release_checker = GitHubReleaseChecker(dialog)
    dialog._release_checker = release_checker

    def finish_check():
        check_update_btn.setEnabled(True)
        check_update_btn.setText(dialog.tr("Check for Updates"))

    def show_release(release: ReleaseInfo):
        finish_check()
        if not is_newer_version(release.tag_name, APP_VERSION):
            message = dialog.tr("You're using the latest version (%1).").replace(
                "%1", APP_VERSION
            )
            show_info_dialog(dialog, dialog.tr("Update Check"), message)
            return

        notes = release.notes or dialog.tr("No release notes provided.")
        content = "\n".join(
            (
                dialog.tr("Current version: %1").replace("%1", APP_VERSION),
                dialog.tr("Latest version: %1").replace("%1", release.tag_name),
                "",
                dialog.tr("What's new:"),
                notes,
            )
        )
        show_update_dialog(
            dialog,
            dialog.tr("Update Available"),
            content,
            dialog.tr("Download:"),
            release.url,
            dialog.tr("Open Download Page"),
        )

    def show_check_error(reason: str):
        finish_check()
        log_warning(T("检查更新失败: {reason}", reason=reason), "Update")
        show_warning_dialog(
            dialog,
            dialog.tr("Update Check Failed"),
            dialog.tr(
                "Unable to check for updates. Please check your network connection "
                "and try again."
            ),
        )

    def start_check():
        if not release_checker.check():
            return
        check_update_btn.setEnabled(False)
        check_update_btn.setText(dialog.tr("Checking..."))

    release_checker.release_found.connect(show_release)
    release_checker.failed.connect(show_check_error)
    check_update_btn.clicked.connect(start_check)

    # 开发者卡片
    dev_card = SettingCard(
        FluentIcon.PEOPLE,
        dialog.tr("Developer"),
        "JYAARU",
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
    license_card.hBoxLayout.addSpacing(dialog_scaled(16))
    group.addSettingCard(license_card)

    # GitHub 链接卡片
    github_card = SettingCard(
        FluentIcon.GITHUB,
        dialog.tr("Source Code"),
        "github.com/1003129155/jietuba",
        parent=group,
    )
    link_btn = HyperlinkButton(
        url=PROJECT_GITHUB_URL,
        text=dialog.tr("Open GitHub"),
        parent=github_card,
    )
    github_card.hBoxLayout.addWidget(link_btn, 0, Qt.AlignmentFlag.AlignRight)
    github_card.hBoxLayout.addSpacing(dialog_scaled(16))
    group.addSettingCard(github_card)

    layout.addWidget(group)
    layout.addStretch(1)
    scroll.setWidget(view)
    return scroll

 
