# -*- coding: utf-8 -*-
"""
第6页 — 完成

庆祝动画 + 配置摘要 + 完成按钮提示
"""

from PySide6.QtWidgets import QVBoxLayout, QLabel, QWidget
from PySide6.QtCore import Qt, QTimer, QPropertyAnimation, QEasingCurve
from PySide6.QtGui import QPainter, QColor, QFont, QPen
from core import safe_event
from core.logger import log_info, log_exception

if __package__:
    from .base_page import BasePage, IllustrationArea, ToggleSwitch, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_PAGE
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import BasePage, IllustrationArea, ToggleSwitch, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_PAGE


# ── 插画区：烟花 / 勾选动画 ─────────────────────────────
class _FinishIllus(IllustrationArea):
    def _build_content(self):
        from PySide6.QtWidgets import QSizePolicy
        self._anim = _CheckAnim(self)
        self._anim.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self._layout.addWidget(self._anim)


class _Particle:
    """简单粒子"""
    def __init__(self, x, y, vx, vy, color, life=1.0):
        self.x, self.y = float(x), float(y)
        self.vx, self.vy = float(vx), float(vy)
        self.color = color
        self.life = life
        self.decay = 0.018 + (abs(vx) + abs(vy)) * 0.003


class _CheckAnim(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        self._particles: list[_Particle] = []
        self._check_progress = 0.0    # 0→1 勾号绘制进度
        self._circle_progress = 0.0   # 0→1 外圆绘制进度
        self._burst_done = False

        self._phase = 0  # 0=circle, 1=check, 2=burst, 3=idle
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(16)

    def _tick(self):
        if self._phase == 0:
            self._circle_progress = min(1.0, self._circle_progress + 0.035)
            if self._circle_progress >= 1.0:
                self._phase = 1
        elif self._phase == 1:
            self._check_progress = min(1.0, self._check_progress + 0.06)
            if self._check_progress >= 1.0:
                self._phase = 2
                self._emit_burst()
        elif self._phase == 2:
            self._update_particles()
            if not self._particles:
                self._phase = 3
                self._timer.stop()
        self.update()

    def _emit_burst(self):
        import math, random
        cx, cy = self.width() / 2, self.height() / 2
        colors = ["#F44336", "#9C27B0", "#2196F3", "#4CAF50", "#FF9800", "#00BCD4"]
        for i in range(48):
            angle = (i / 48) * math.pi * 2 + random.uniform(-0.1, 0.1)
            speed = random.uniform(2.5, 5.5)
            vx = math.cos(angle) * speed
            vy = math.sin(angle) * speed
            c = random.choice(colors)
            self._particles.append(_Particle(cx, cy, vx, vy, c, 1.0))

    def _update_particles(self):
        alive = []
        for pt in self._particles:
            pt.x += pt.vx
            pt.y += pt.vy
            pt.vy += 0.12  # gravity
            pt.life -= pt.decay
            if pt.life > 0:
                alive.append(pt)
        self._particles = alive

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        cx, cy = w / 2, h / 2
        r = min(w, h) * 0.22

        # 外圆（描边弧）
        if self._circle_progress > 0:
            import math
            span = int(self._circle_progress * 360 * 16)
            pen = QPen(QColor(ACCENT), 4)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            p.setPen(pen)
            p.setBrush(Qt.GlobalColor.transparent)
            p.drawArc(
                int(cx - r), int(cy - r), int(r * 2), int(r * 2),
                90 * 16, -span
            )

        # 填充圆背景（circle_progress=1 时）
        if self._circle_progress >= 1.0:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(33, 150, 243, 30))
            p.drawEllipse(int(cx - r), int(cy - r), int(r * 2), int(r * 2))

        # 勾号
        if self._check_progress > 0:
            # 勾号三个点：起点→拐点→终点
            x1, y1 = cx - r * 0.38, cy
            x2, y2 = cx - r * 0.05, cy + r * 0.38
            x3, y3 = cx + r * 0.45, cy - r * 0.32

            pen = QPen(QColor(ACCENT), 4)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            p.setPen(pen)

            # 分两段绘制
            seg1_len = 0.4  # 第1段占比
            t = self._check_progress
            if t <= seg1_len:
                # 绘第1段部分
                frac = t / seg1_len
                mx = x1 + (x2 - x1) * frac
                my = y1 + (y2 - y1) * frac
                p.drawLine(int(x1), int(y1), int(mx), int(my))
            else:
                # 第1段完整
                p.drawLine(int(x1), int(y1), int(x2), int(y2))
                # 绘第2段部分
                frac = (t - seg1_len) / (1.0 - seg1_len)
                mx = x2 + (x3 - x2) * frac
                my = y2 + (y3 - y2) * frac
                p.drawLine(int(x2), int(y2), int(mx), int(my))

        # 粒子
        for pt in self._particles:
            alpha = max(0, int(pt.life * 220))
            c = QColor(pt.color)
            c.setAlpha(alpha)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(c)
            p.drawEllipse(int(pt.x) - 3, int(pt.y) - 3, 6, 6)


# ── 页面主体 ─────────────────────────────────────────────
class FinishPage(BasePage):
    """第6页：完成"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title=self.tr("🎉 一切就绪！"),
            subtitle=self.tr(
                "你已完成基础设置，截图工具已准备好为你服务。\n"
                "随时可以在设置面板调整更多选项。"),
            parent=parent,
        )

    def _create_illustration(self):
        return _FinishIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        # ── 开机自启 ──────────────────────────────────────
        self._autostart_switch = ToggleSwitch()
        self._autostart_switch.setChecked(self._get_autostart())
        row_auto, self._autostart_lbl, self._autostart_desc = \
            self._make_setting_row_with_refs(
                self.tr("开机自启"),
                self._autostart_switch,
                self.tr("开机后在检测更新后启动。")
            )
        layout.addWidget(row_auto)

        # ── 启动时显示主界面 ──────────────────────────────
        self._show_main_switch = ToggleSwitch()
        self._show_main_switch.setChecked(self._config.get_show_main_window())
        row_show, self._show_main_lbl, self._show_main_desc = \
            self._make_setting_row_with_refs(
                self.tr("启动时显示主界面"),
                self._show_main_switch,
                self.tr("每次启动时自动打开设置面板。")
            )
        layout.addWidget(row_show)

        # ── 生成桌面快捷方式 ──────────────────────────────
        self._desktop_switch = ToggleSwitch()
        self._desktop_switch.setChecked(True)  # 默认开启
        row_desktop, self._desktop_lbl, self._desktop_desc = \
            self._make_setting_row_with_refs(
                self.tr("生成桌面快捷方式"),
                self._desktop_switch,
                self.tr("完成向导时在桌面创建截图吧快捷方式。")
            )
        layout.addWidget(row_desktop)

    # ── 开机自启辅助 ─────────────────────────────────────

    _LNK_NAME = "start_jietuba.lnk"
    _LNK_LOCAL = r"C:\jietuba\start_jietuba.lnk"
    _LNK_NETWORK = r"\\honbu02s\organ\SYSTEM\共有_部内専用\951_RiJyaaru\jietuba\start_jietuba.lnk"

    @classmethod
    def _get_startup_lnk_path(cls) -> str:
        """返回 Windows 启动文件夹中快捷方式的完整路径"""
        import os
        startup = os.path.join(
            os.environ.get("APPDATA", ""),
            r"Microsoft\Windows\Start Menu\Programs\Startup",
            cls._LNK_NAME,
        )
        return startup

    @classmethod
    def _get_autostart(cls) -> bool:
        """检测 startup 文件夹中是否存在快捷方式"""
        import os
        return os.path.exists(cls._get_startup_lnk_path())

    @classmethod
    def _set_autostart(cls, enabled: bool):
        """启用：将快捷方式复制到 startup；禁用：删除 startup 中的快捷方式"""
        import os, shutil
        startup_lnk = cls._get_startup_lnk_path()
        if enabled:
            # 优先使用本地文件，本地不存在时尝试网络备用路径
            src = None
            if os.path.exists(cls._LNK_LOCAL):
                src = cls._LNK_LOCAL
            elif os.path.exists(cls._LNK_NETWORK):
                src = cls._LNK_NETWORK
            if src is None:
                # 找不到源文件，静默跳过，不报错
                return
            try:
                os.makedirs(os.path.dirname(startup_lnk), exist_ok=True)
                shutil.copy2(src, startup_lnk)
                log_info(f"已复制快捷方式到 startup: {startup_lnk}", "page6")
            except Exception as e:
                log_exception(e, "设置开机自启")
        else:
            try:
                if os.path.exists(startup_lnk):
                    os.remove(startup_lnk)
                    log_info(f"已删除 startup 快捷方式: {startup_lnk}", "page6")
            except Exception as e:
                log_exception(e, "删除开机自启")

    # ── 桌面快捷方式辅助 ─────────────────────────────────

    @classmethod
    def _get_desktop_lnk_path(cls) -> str:
        """返回桌面上快捷方式的完整路径"""
        import os
        desktop = os.path.join(os.path.expanduser("~"), "Desktop", cls._LNK_NAME)
        return desktop

    @classmethod
    def _create_desktop_shortcut(cls):
        """将快捷方式复制到桌面"""
        import os, shutil
        desktop_lnk = cls._get_desktop_lnk_path()
        src = None
        if os.path.exists(cls._LNK_LOCAL):
            src = cls._LNK_LOCAL
        elif os.path.exists(cls._LNK_NETWORK):
            src = cls._LNK_NETWORK
        if src is None:
            # 找不到源文件，静默跳过，不创建桌面快捷方式
            return
        try:
            shutil.copy2(src, desktop_lnk)
            log_info(f"已创建桌面快捷方式: {desktop_lnk}", "page6")
        except Exception as e:
            log_exception(e, "创建桌面快捷方式")

    def retranslate(self):
        self.title_label.setText(self.tr("🎉 一切就绪！"))
        self.subtitle_label.setText(self.tr(
            "你已完成基础设置，截图工具已准备好为你服务。\n"
            "随时可以在设置面板调整更多选项。"))
        if hasattr(self, "_autostart_lbl") and self._autostart_lbl:
            self._autostart_lbl.setText(self.tr("开机自启"))
        if hasattr(self, "_autostart_desc") and self._autostart_desc:
            self._autostart_desc.setText(self.tr("开机后在检测更新后启动。"))
        if hasattr(self, "_show_main_lbl") and self._show_main_lbl:
            self._show_main_lbl.setText(self.tr("启动时显示主界面"))
        if hasattr(self, "_show_main_desc") and self._show_main_desc:
            self._show_main_desc.setText(self.tr("每次启动时自动打开设置面板。"))
        if hasattr(self, "_desktop_lbl") and self._desktop_lbl:
            self._desktop_lbl.setText(self.tr("生成桌面快捷方式"))
        if hasattr(self, "_desktop_desc") and self._desktop_desc:
            self._desktop_desc.setText(self.tr("完成向导时在桌面创建截图吧快捷方式。"))

    def save(self):
        """保存设置：标记向导完成 + 写入主界面偏好；
        文件操作（开机自启、桌面快捷方式）放到后台线程，避免阻塞主线程。"""
        if hasattr(self._config, "set_app_setting"):
            self._config.set_app_setting("welcome_wizard_done", "1")
        if hasattr(self, "_show_main_switch"):
            self._config.set_show_main_window(self._show_main_switch.isChecked())

        # 文件/网络路径操作放到后台线程，避免 UNC 路径探测阻塞主线程
        autostart_on = hasattr(self, "_autostart_switch") and self._autostart_switch.isChecked()
        desktop_on   = hasattr(self, "_desktop_switch")   and self._desktop_switch.isChecked()

        import threading
        def _bg():
            if autostart_on:
                self._set_autostart(True)
            elif hasattr(self, "_autostart_switch"):
                self._set_autostart(False)
            if desktop_on:
                self._create_desktop_shortcut()

        threading.Thread(target=_bg, daemon=True).start()


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(5)   # 跳到第6页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 