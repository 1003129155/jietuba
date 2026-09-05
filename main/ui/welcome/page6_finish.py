# -*- coding: utf-8 -*-
"""
第6页 — 完成

克制的完成动画 + 最终设置
"""

import math

from PySide6.QtWidgets import QVBoxLayout, QWidget
from PySide6.QtCore import Qt, QTimer, QElapsedTimer, QEasingCurve, QPointF
from PySide6.QtGui import QPainter, QColor, QPen, QPainterPath
from core import safe_event
from core.i18n import make_tr
from core.logger import log_info, log_exception, T

if __package__:
    from .base_page import (
        BasePage, IllustrationArea, ToggleSwitch, PRODUCT_NAME, brand_text, welcome_theme,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        BasePage, IllustrationArea, ToggleSwitch, PRODUCT_NAME, brand_text, welcome_theme,
    )


_tr = make_tr("WelcomeWizard")


# ── 插画区：一次性完成动画 ──────────────────────────────
class _FinishIllus(IllustrationArea):
    def _build_content(self):
        from PySide6.QtWidgets import QSizePolicy
        self._anim = _CheckAnim(self)
        self._anim.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self._layout.addWidget(self._anim)

    def _apply_welcome_theme(self, _tokens=None):
        """完成徽记直接悬浮在页面上，不再套一张巨大的空卡片。"""
        self.setStyleSheet("""
            #IllustrationArea {
                background: transparent;
                border: none;
            }
        """)
        self.update()


class _CheckAnim(QWidget):
    """实心圆、路径勾、双层波纹和少量光点组成的一次性完成动效。"""

    DURATION_MS = 1120

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        self._elapsed = QElapsedTimer()
        self._time_ms = self.DURATION_MS
        self._has_been_shown = False
        self._timer = QTimer(self)
        self._timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._timer.timeout.connect(self._tick)

    def _tick(self):
        self._time_ms = min(self.DURATION_MS, self._elapsed.elapsed())
        if self._time_ms >= self.DURATION_MS:
            self._timer.stop()
        self.update()

    def play(self):
        self._time_ms = 0
        self._elapsed.restart()
        self._timer.start(16)
        self.update()

    def showEvent(self, event):
        super().showEvent(event)
        # 页面真正进入可见状态时播放，避免向导初始化期间提前播完。
        if self._has_been_shown:
            self.play()
        else:
            self._has_been_shown = True
            QTimer.singleShot(90, self.play)

    @staticmethod
    def _progress(now: float, start: float, end: float) -> float:
        if end <= start:
            return 1.0
        return max(0.0, min(1.0, (now - start) / (end - start)))

    @staticmethod
    def _ease(curve: QEasingCurve.Type, value: float) -> float:
        return QEasingCurve(curve).valueForProgress(value)

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        theme = welcome_theme()
        center = QPointF(w / 2.0, h / 2.0)
        now = float(self._time_ms)
        base_r = min(44.0, max(34.0, min(w, h) * 0.13))

        circle_t = self._progress(now, 70, 410)
        circle_scale = self._ease(QEasingCurve.Type.OutBack, circle_t)
        check_t = self._ease(
            QEasingCurve.Type.OutCubic, self._progress(now, 330, 690)
        )
        effects_t = self._progress(now, 560, 1040)

        # 最终仍保留的柔和底光，填补空白但不形成另一只“大圆”。
        glow = QColor(theme.accent)
        glow.setAlpha(12 if theme.is_dark else 9)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(glow)
        p.drawEllipse(center, base_r * 1.9, base_r * 1.9)

        # 两圈只出现一次的确认波纹。
        for delay, max_scale in ((0.0, 1.85), (0.17, 2.15)):
            wave_t = max(0.0, min(1.0, (effects_t - delay) / (1.0 - delay)))
            if 0.0 < wave_t < 1.0:
                wave_r = base_r * (1.0 + (max_scale - 1.0) * wave_t)
                wave_color = QColor(theme.accent)
                wave_color.setAlpha(int(54 * (1.0 - wave_t) ** 2))
                wave_pen = QPen(wave_color, 1.5)
                p.setPen(wave_pen)
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawEllipse(center, wave_r, wave_r)

        # 少量同色光点，从徽记边缘短距离散开并淡出。
        if 0.0 < effects_t < 1.0:
            spark_color = QColor(theme.accent)
            spark_color.setAlpha(int(175 * math.sin(math.pi * effects_t)))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(spark_color)
            angles = (-148, -92, -34, 20, 78, 142)
            for index, angle_deg in enumerate(angles):
                angle = math.radians(angle_deg)
                distance = base_r * (1.28 + effects_t * (0.30 + index % 2 * 0.10))
                point = QPointF(
                    center.x() + math.cos(angle) * distance,
                    center.y() + math.sin(angle) * distance,
                )
                dot_r = 1.8 if index % 2 else 2.4
                p.drawEllipse(point, dot_r, dot_r)

        # 小型实心完成徽记。
        radius = base_r * circle_scale
        if radius > 0.1:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(theme.accent))
            p.drawEllipse(center, radius, radius)

        # 白色勾按路径分两段绘制。
        if check_t > 0.0 and circle_scale > 0.0:
            r = base_r
            start = QPointF(center.x() - r * 0.45, center.y() - r * 0.01)
            corner = QPointF(center.x() - r * 0.10, center.y() + r * 0.35)
            end = QPointF(center.x() + r * 0.50, center.y() - r * 0.34)
            check_path = QPainterPath(start)
            first_part = 0.37
            if check_t <= first_part:
                frac = check_t / first_part
                check_path.lineTo(
                    start.x() + (corner.x() - start.x()) * frac,
                    start.y() + (corner.y() - start.y()) * frac,
                )
            else:
                check_path.lineTo(corner)
                frac = (check_t - first_part) / (1.0 - first_part)
                check_path.lineTo(
                    corner.x() + (end.x() - corner.x()) * frac,
                    corner.y() + (end.y() - corner.y()) * frac,
                )

            check_pen = QPen(QColor("#FFFFFF"), max(4.0, base_r * 0.105))
            check_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            check_pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            p.setPen(check_pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(check_path)


# ── 页面主体 ─────────────────────────────────────────────
class FinishPage(BasePage):
    """第6页：完成"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title=_tr("🎉 一切就绪！").replace("🎉", "").strip(),
            subtitle=_tr(
                "你已完成基础设置，截图工具已准备好为你服务。\n"
                "随时可以在设置面板调整更多选项。"),
            parent=parent,
        )

    def _create_illustration(self):
        return _FinishIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        # ── 开机自启 ──────────────────────────────────────
        self._autostart_switch = ToggleSwitch()
        self._autostart_switch.setChecked(True)  # 欢迎向导默认开启
        row_auto, self._autostart_lbl, self._autostart_desc = \
            self._make_setting_row_with_refs(
                _tr("开机自启"),
                self._autostart_switch,
                _tr("开机后在检测更新后启动。")
            )
        layout.addWidget(row_auto)

        # ── 启动时显示主界面 ──────────────────────────────
        self._show_main_switch = ToggleSwitch()
        self._show_main_switch.setChecked(self._config.get_show_main_window())
        row_show, self._show_main_lbl, self._show_main_desc = \
            self._make_setting_row_with_refs(
                _tr("启动时显示主界面"),
                self._show_main_switch,
                _tr("每次启动时自动打开设置面板。")
            )
        layout.addWidget(row_show)

        # ── 桌面快捷方式 ──────────────────────────────────
        self._desktop_switch = ToggleSwitch()
        self._desktop_switch.setChecked(True)  # 默认开启
        row_desktop, self._desktop_lbl, self._desktop_desc = \
            self._make_setting_row_with_refs(
                _tr("快捷方式"),
                self._desktop_switch,
                brand_text(_tr("完成向导时在桌面创建截图吧快捷方式。"))
            )
        layout.addWidget(row_desktop)

    # ── 开机自启辅助（注册表 HKCU\Run 方案）─────────────────

    _AUTOSTART_REG_KEY = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run"
    _AUTOSTART_APP_NAME = "Jietuba"

    @classmethod
    def _get_exe_path(cls) -> str:
        """获取当前运行的可执行文件路径。
        打包后（PyInstaller frozen）返回 .exe 路径；
        开发模式下返回 python.exe + 主脚本路径。
        """
        import sys, os
        if getattr(sys, 'frozen', False):
            return sys.executable
        # 开发模式：python.exe main/main_app.py
        main_script = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "..", "main_app.py")
        )
        return f'"{sys.executable}" "{main_script}"'

    @classmethod
    def _get_autostart(cls) -> bool:
        """检测注册表 HKCU\\Run 中是否存在本程序的启动项"""
        import winreg
        try:
            key = winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                cls._AUTOSTART_REG_KEY,
                0,
                winreg.KEY_READ,
            )
            try:
                winreg.QueryValueEx(key, cls._AUTOSTART_APP_NAME)
                return True
            except FileNotFoundError:
                return False
            finally:
                winreg.CloseKey(key)
        except Exception:
            return False

    @classmethod
    def _set_autostart(cls, enabled: bool):
        """启用：写入注册表 HKCU\\Run；禁用：删除对应注册表值"""
        import winreg
        try:
            key = winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                cls._AUTOSTART_REG_KEY,
                0,
                winreg.KEY_SET_VALUE,
            )
            if enabled:
                exe_path = cls._get_exe_path()
                winreg.SetValueEx(key, cls._AUTOSTART_APP_NAME, 0, winreg.REG_SZ, exe_path)
                log_info(T("已写入开机自启注册表项: {exe_path}", exe_path=exe_path), "page6")
            else:
                try:
                    winreg.DeleteValue(key, cls._AUTOSTART_APP_NAME)
                    log_info(T("已删除开机自启注册表项"), "page6")
                except FileNotFoundError:
                    pass  # 不存在则忽略
            winreg.CloseKey(key)
        except Exception as e:
            log_exception(e, T("设置开机自启"))

    # ── 桌面快捷方式辅助 ─────────────────────────────────

    _DESKTOP_LNK_NAME = f"{PRODUCT_NAME}.lnk"

    @classmethod
    def _get_desktop_lnk_path(cls) -> str:
        """返回桌面上快捷方式的完整路径"""
        import os
        desktop = os.path.join(os.path.expanduser("~"), "Desktop", cls._DESKTOP_LNK_NAME)
        return desktop

    @classmethod
    def _create_desktop_shortcut(cls):
        """在桌面创建指向当前程序的快捷方式。"""
        import base64
        import os
        import subprocess
        import sys

        desktop_lnk = cls._get_desktop_lnk_path()
        try:
            if getattr(sys, 'frozen', False):
                target_path = sys.executable
                arguments = ""
                working_directory = os.path.dirname(sys.executable)
            else:
                main_script = os.path.abspath(
                    os.path.join(os.path.dirname(__file__), "..", "..", "main_app.py")
                )
                target_path = sys.executable
                arguments = f'"{main_script}"'
                working_directory = os.path.dirname(main_script)

            def _ps_quote(value: str) -> str:
                return "'" + value.replace("'", "''") + "'"

            script = "\n".join([
                f"$shortcutPath = {_ps_quote(desktop_lnk)}",
                f"$targetPath = {_ps_quote(target_path)}",
                f"$workingDirectory = {_ps_quote(working_directory)}",
                f"$arguments = {_ps_quote(arguments)}",
                "$shell = New-Object -ComObject WScript.Shell",
                "$shortcut = $shell.CreateShortcut($shortcutPath)",
                "$shortcut.TargetPath = $targetPath",
                "$shortcut.WorkingDirectory = $workingDirectory",
                "if ($arguments.Length -gt 0) { $shortcut.Arguments = $arguments }",
                "$shortcut.IconLocation = $targetPath",
                "$shortcut.Save()",
            ])
            encoded_script = base64.b64encode(script.encode("utf-16le")).decode("ascii")
            result = subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-EncodedCommand",
                    encoded_script,
                ],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            if result.returncode != 0:
                error_text = (result.stderr or result.stdout or "").strip()
                raise RuntimeError(error_text or f"PowerShell exited with code {result.returncode}")

            log_info(T("已创建桌面快捷方式: {desktop_lnk}", desktop_lnk=desktop_lnk), "page6")
        except Exception as e:
            log_exception(e, T("创建桌面快捷方式"))

    def retranslate(self):
        self.title_label.setText(_tr("🎉 一切就绪！").replace("🎉", "").strip())
        self.subtitle_label.setText(_tr(
            "你已完成基础设置，截图工具已准备好为你服务。\n"
            "随时可以在设置面板调整更多选项。"))
        if hasattr(self, "_autostart_lbl") and self._autostart_lbl:
            self._autostart_lbl.setText(_tr("开机自启"))
        if hasattr(self, "_autostart_desc") and self._autostart_desc:
            self._autostart_desc.setText(_tr("开机后在检测更新后启动。"))
        if hasattr(self, "_show_main_lbl") and self._show_main_lbl:
            self._show_main_lbl.setText(_tr("启动时显示主界面"))
        if hasattr(self, "_show_main_desc") and self._show_main_desc:
            self._show_main_desc.setText(_tr("每次启动时自动打开设置面板。"))
        if hasattr(self, "_desktop_lbl") and self._desktop_lbl:
            self._desktop_lbl.setText(_tr("快捷方式"))
        if hasattr(self, "_desktop_desc") and self._desktop_desc:
            self._desktop_desc.setText(
                brand_text(_tr("完成向导时在桌面创建截图吧快捷方式。"))
            )

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
 
