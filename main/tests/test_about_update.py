from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtWidgets import QDialog, QPushButton

from core.constants import PROJECT_RELEASES_LATEST_URL
from core.update_checker import ReleaseInfo
from ui.settings_ui import page_about


class FakeReleaseChecker(QObject):
    release_found = Signal(object)
    failed = Signal(str)
    instances = []

    def __init__(self, parent=None):
        super().__init__(parent)
        self.started = False
        self.instances.append(self)

    def check(self):
        self.started = True
        return True


def _create_page(monkeypatch, qapp):
    FakeReleaseChecker.instances.clear()
    monkeypatch.setattr(page_about, "GitHubReleaseChecker", FakeReleaseChecker)
    dialog = QDialog()
    page = page_about.create_about_page(dialog)
    button = page.findChild(QPushButton, "checkUpdateButton")
    return dialog, page, button, FakeReleaseChecker.instances[-1]


def test_check_button_reports_when_current_version_is_latest(monkeypatch, qapp, qtbot):
    info_calls = []
    monkeypatch.setattr(
        page_about,
        "show_info_dialog",
        lambda *args: info_calls.append(args),
    )
    dialog, page, button, checker = _create_page(monkeypatch, qapp)

    qtbot.mouseClick(button, Qt.MouseButton.LeftButton)
    # 按钮挂在卡片的控件列容器里，往上两层才是卡片
    version_card = button.parentWidget().parentWidget()
    assert version_card.titleLabel.text() == "Version"
    assert version_card.contentLabel.text() == page_about.APP_VERSION
    assert checker.started
    assert not button.isEnabled()
    assert button.text() == "Checking..."

    checker.release_found.emit(
        ReleaseInfo("release-2.0", "Release 2.0", "Notes", PROJECT_RELEASES_LATEST_URL)
    )

    assert button.isEnabled()
    assert button.text() == "Check for Updates"
    assert info_calls[0][1] == "Update Check"
    assert page_about.APP_VERSION in info_calls[0][2]
    page.deleteLater()
    dialog.deleteLater()


def test_new_release_shows_notes_and_download_address(monkeypatch, qapp, qtbot):
    update_calls = []
    monkeypatch.setattr(
        page_about,
        "show_update_dialog",
        lambda *args: update_calls.append(args),
    )
    dialog, page, button, checker = _create_page(monkeypatch, qapp)

    qtbot.mouseClick(button, Qt.MouseButton.LeftButton)
    checker.release_found.emit(
        ReleaseInfo(
            "v2.1.0",
            "Version 2.1",
            "Added update checking.",
            PROJECT_RELEASES_LATEST_URL,
        )
    )

    assert len(update_calls) == 1
    _, title, content, download_caption, url, action_text = update_calls[0]
    assert title == "Update Available"
    assert f"Current version: {page_about.APP_VERSION}" in content
    assert "Latest version: v2.1.0" in content
    assert "Added update checking." in content
    assert download_caption == "Download:"
    assert url == PROJECT_RELEASES_LATEST_URL
    assert action_text == "Open Download Page"
    page.deleteLater()
    dialog.deleteLater()


def test_failed_check_restores_button_and_shows_warning(monkeypatch, qapp, qtbot):
    warning_calls = []
    monkeypatch.setattr(page_about, "log_warning", lambda *args: None)
    monkeypatch.setattr(
        page_about,
        "show_warning_dialog",
        lambda *args: warning_calls.append(args),
    )
    dialog, page, button, checker = _create_page(monkeypatch, qapp)

    qtbot.mouseClick(button, Qt.MouseButton.LeftButton)
    checker.failed.emit("timed out")

    assert button.isEnabled()
    assert button.text() == "Check for Updates"
    assert warning_calls[0][1] == "Update Check Failed"
    page.deleteLater()
    dialog.deleteLater()
