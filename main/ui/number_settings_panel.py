"""
序号工具设置面板
适用于：序号 (number)
"""
from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QLabel, QToolButton, QVBoxLayout, QWidget

from .base_settings_panel import BaseSettingsPanel, PANEL_SCALE


class NumberSettingsPanel(BaseSettingsPanel):
    """序号工具设置面板 - 统一风格"""

    next_number_changed = Signal(int)

    SIZE_RANGE = (8, 72)
    SIZE_DEFAULT = 16
    SIZE_TOOLTIP = "Font Size"

    def _build_extra_controls(self, layout):
        _sz = round(26 * PANEL_SCALE)
        self.next_preview = QLabel()
        self.next_preview.setFixedSize(_sz, _sz)
        self.next_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        _r = _sz // 2
        self.next_preview.setStyleSheet(
            f"QLabel {{ background: transparent; color: #333; border: 2px solid #666; border-radius: {_r}px; font-weight: bold; }}"
        )
        layout.insertWidget(0, self.next_preview)

        self._next_value = 1

        btn_wrap = QWidget()
        btn_layout = QVBoxLayout(btn_wrap)
        btn_layout.setContentsMargins(0, 0, 0, 0)
        btn_layout.setSpacing(2)

        self.next_up_btn = QToolButton()
        self.next_up_btn.setArrowType(Qt.ArrowType.UpArrow)
        self.next_up_btn.setFixedSize(round(18 * PANEL_SCALE), round(14 * PANEL_SCALE))
        self.next_up_btn.setToolTip(self._tr("Next Number"))
        btn_layout.addWidget(self.next_up_btn)

        self.next_down_btn = QToolButton()
        self.next_down_btn.setArrowType(Qt.ArrowType.DownArrow)
        self.next_down_btn.setFixedSize(round(18 * PANEL_SCALE), round(14 * PANEL_SCALE))
        self.next_down_btn.setToolTip(self._tr("Next Number"))
        btn_layout.addWidget(self.next_down_btn)

        layout.insertWidget(1, btn_wrap)

        self.next_up_btn.clicked.connect(lambda: self._change_next_number(1))
        self.next_down_btn.clicked.connect(lambda: self._change_next_number(-1))
        self._update_next_preview(self._next_value)

    def set_next_number(self, value: int):
        if not hasattr(self, "next_preview"):
            return
        value = max(1, int(value))
        if getattr(self, "_next_value", 1) == value:
            return
        self._next_value = value
        self._update_next_preview(value)

    def _update_next_preview(self, value: int):
        if not hasattr(self, "next_preview"):
            return
        self.next_preview.setText(str(int(value)))

    def _change_next_number(self, delta: int):
        value = max(1, int(getattr(self, "_next_value", 1)) + int(delta))
        self._next_value = value
        self._update_next_preview(value)
        self.next_number_changed.emit(int(value))
 