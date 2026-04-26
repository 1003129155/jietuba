# -*- coding: utf-8 -*-
"""
第1页 — 欢迎 & 语言选择

上半部：大标志 + 欢迎文字动画
下半部：标题 + 描述 + 语言选择；右下角叠放 wizard.png
"""

from PySide6.QtWidgets import QVBoxLayout, QHBoxLayout, QLabel, QWidget
from PySide6.QtCore import Qt, QTimer, QPropertyAnimation, QEasingCurve, Property
from PySide6.QtGui import QFont, QColor, QPainter, QPixmap, QIcon
from qfluentwidgets import ComboBox

from core.i18n import make_tr
from core.logger import log_exception
from core import safe_event

if __package__:
    from .base_page import BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_ILLUS
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_ILLUS


_tr = make_tr("WelcomeWizard")


# ── 插画区：大 Logo + 打字机标语 ───────────────────────
class _WelcomeIllus(IllustrationArea):
    def _build_content(self):
        from PySide6.QtWidgets import QSizePolicy

        # Logo 图标
        self._icon_lbl = QLabel()
        self._icon_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._icon_lbl.setStyleSheet("background: transparent;")
        self._try_load_icon()
        self._layout.addWidget(self._icon_lbl)

        # 应用名
        name_lbl = QLabel("截图吧")
        name_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        name_lbl.setStyleSheet(
            f"font-size: 28px; font-weight: 800; color: {ACCENT};"
            " background: transparent; letter-spacing: 4px;"
        )
        self._layout.addWidget(name_lbl)

        # 打字机副标题
        self._tagline_lbl = QLabel("")
        self._tagline_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._tagline_lbl.setStyleSheet(
            f"font-size: 13px; color: {TEXT_SECOND}; background: transparent;"
        )
        self._layout.addWidget(self._tagline_lbl)

        # 打字机效果（文字由 retranslate 设置，首次在此读取当前翻译）
        self._type_idx = 0
        self._type_timer = QTimer(self)
        self._type_timer.timeout.connect(self._tick_typewriter)
        self._type_start_timer = QTimer(self)
        self._type_start_timer.setSingleShot(True)
        self._type_start_timer.timeout.connect(self._start_typewriter)
        self._reset_typewriter(400)

    def retranslate(self):
        """语言切换后重置打字机文字并重新播放。"""
        self._reset_typewriter(200)

    def _reset_typewriter(self, delay_ms: int):
        self._full_text = _tr("截图 · 标注 · 剪贴板 · 翻译，一站搞定")
        # 重置进度并清空标签，让打字机重新播放
        self._type_start_timer.stop()
        self._type_timer.stop()
        self._type_idx = 0
        self._tagline_lbl.setText("")
        self._type_start_timer.start(delay_ms)

    def _start_typewriter(self):
        if self._type_idx < len(self._full_text):
            self._type_timer.start(45)

    def _try_load_icon(self):
        try:
            from core.resource_manager import ResourceManager
            import os
            path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(path):
                px = QIcon(path).pixmap(72, 72)
                self._icon_lbl.setPixmap(px)
                return
        except Exception as e:
            log_exception(e, "加载托盘图标")
        self._icon_lbl.setText("🖼")
        self._icon_lbl.setStyleSheet(
            "font-size: 48px; background: transparent;"
        )

    def _tick_typewriter(self):
        self._type_idx += 1
        self._tagline_lbl.setText(self._full_text[:self._type_idx])
        if self._type_idx >= len(self._full_text):
            self._type_timer.stop()


# ── 页面主体 ────────────────────────────────────────────
class WelcomePage(BasePage):
    """第1页：欢迎 + 语言选择"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager

        # 读取当前已加载的语言（由 WelcomeWizard._init_language 提前设置好）
        try:
            from core.i18n import I18nManager
            self._init_lang = I18nManager.get_current_language()
        except Exception:
            self._init_lang = "ja"

        super().__init__(
            title="欢迎使用截图吧 👋",
            subtitle="一款轻巧的屏幕截图与剪贴板管理工具，\n"
                     "由rijyaaru用 Python + PySide6 打造。\n"
                     "先选好语言，再开始探索吧！",
            parent=parent,
        )
        # 在内容区右侧叠放 wizard.png（绝对定位，不影响原布局）
        self._wizard_img_lbl = QLabel(self)
        self._wizard_img_lbl.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._wizard_img_lbl.setStyleSheet("background: transparent;")
        self._try_load_wizard_img()

    # ── 插画 ────────────────────────────────────────────
    def _create_illustration(self):
        return _WelcomeIllus(self)

    # ── 控件 ────────────────────────────────────────────
    def _build_controls(self, layout: QVBoxLayout):
        # 语言标签（靠左）
        self._row_lang_lbl = QLabel("🌐 界面语言")
        self._row_lang_lbl.setStyleSheet(
            f"font-size: 14px; font-weight: 600; color: {TEXT_PRIMARY};"
            " background: transparent;"
        )
        layout.addWidget(self._row_lang_lbl)

        # 下拉框（靠左，固定宽度）
        self._lang_combo = ComboBox()
        self._lang_combo.setFixedWidth(150)
        self._lang_combo.setFixedHeight(32)
        self._lang_combo.setCursor(Qt.CursorShape.PointingHandCursor)

        try:
            from core.i18n import I18nManager
            current = getattr(self, "_init_lang", I18nManager.get_current_language())
            for code, name in I18nManager.get_available_languages().items():
                self._lang_combo.addItem(name, code)
            idx = self._lang_combo.findData(current)
            if idx >= 0:
                self._lang_combo.setCurrentIndex(idx)
        except Exception:
            self._lang_combo.addItem("日本語", "ja")
            self._lang_combo.addItem("English", "en")
            self._lang_combo.addItem("简体中文", "zh")

        self._lang_combo.currentIndexChanged.connect(self._on_lang_changed)

        # 用 HBoxLayout 让下拉框靠左（addStretch 推走右边空白）
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(self._lang_combo)
        row.addStretch()
        layout.addLayout(row)



    def _try_load_wizard_img(self):
        """加载 svg/wizard.png，按比例缩放后叠放到内容区右下角。"""
        try:
            from core.resource_manager import ResourceManager
            import os
            path = ResourceManager.get_resource_path("svg/wizard.png")
            if os.path.exists(path):
                px = QPixmap(path)
                if not px.isNull():
                    target_w = min(200, px.width())
                    px = px.scaledToWidth(target_w, Qt.TransformationMode.SmoothTransformation)
                    self._wizard_img_lbl.setPixmap(px)
                    self._wizard_img_lbl.adjustSize()
                    return
        except Exception as e:
            log_exception(e, "加载向导图片")
        self._wizard_img_lbl.hide()

    @safe_event
    def resizeEvent(self, event):
        """每次大小变化时重新定位 wizard 图片到右下角。"""
        super().resizeEvent(event)
        lbl = getattr(self, "_wizard_img_lbl", None)
        if lbl and lbl.pixmap() and not lbl.pixmap().isNull():
            margin_right = 24
            margin_bottom = 12
            x = self.width() - lbl.width() - margin_right
            y = self.height() - lbl.height() - margin_bottom
            lbl.move(x, y)
            lbl.raise_()

    def retranslate(self):
        """语言切换后由 wizard.retranslate_ui() 调用，刷新本页所有可见文字。"""
        self.title_label.setText(_tr("欢迎使用截图吧 👋"))
        self.subtitle_label.setText(_tr(
            "一款轻巧的屏幕截图与剪贴板管理工具，\n"
            "由rijyaaru用 Python + PySide6 打造。\n"
            "先选好语言，再开始探索吧！"))
        if hasattr(self, "_row_lang_lbl") and self._row_lang_lbl:
            self._row_lang_lbl.setText(_tr("🌐 界面语言"))
        if hasattr(self, "_row_lang_desc") and self._row_lang_desc:
            self._row_lang_desc.setText(_tr("选择后立即生效，欢迎界面将随之切换语言。"))
        # 级联刷新插画区（打字机文字）
        if hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    # ── 逻辑 ────────────────────────────────────────────
    def _on_lang_changed(self):
        code = self._lang_combo.currentData()
        if not code:
            return
        try:
            from core.i18n import I18nManager
            I18nManager.load_language(code)
        except Exception as e:
            log_exception(e, "加载语言")
        # 保存到 config
        self._config.set_app_setting("language", code)
        # 通知父 wizard 刷新所有页面标题（如果父是 WelcomeWizard）
        wizard = self._find_wizard()
        if wizard is not None:
            wizard.retranslate_ui()

    def _find_wizard(self):
        """向上找到 WelcomeWizard 实例（在 QStackedWidget 里）"""
        p = self.parent()
        while p is not None:
            # 避免循环导入，用类名字符串判断
            if type(p).__name__ == "WelcomeWizard":
                return p
            p = p.parent()
        return None

    def save(self):
        """向导结束时调用，持久化设置"""
        pass  # 已在 _on_lang_changed 实时保存


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(0)   # 跳到第1页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 