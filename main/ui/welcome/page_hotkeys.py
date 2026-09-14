# -*- coding: utf-8 -*-
"""欢迎向导：全局快捷键页。

六个全局快捷键（截图、剪贴板、智能翻译各主键 + 备用键）集中在这一页设置。

它们属于同一个冲突域——同一个实际按键绑给两个业务时，第二次注册必定静默
失败，所以六个必须摆在一起、用同一套判重规则校验，用户才有机会一眼看出
彼此不冲突。判重规则本身复用设置窗口那份（ui.hotkey_edit）。
"""
from PySide6.QtWidgets import (
    QApplication, QVBoxLayout, QHBoxLayout, QLabel, QWidget, QSizePolicy,
)
from PySide6.QtCore import Qt, QRectF
from PySide6.QtGui import QPainter, QColor, QPen, QFont

from core.i18n import make_tr
from core.logger import log_exception, T

if __package__:
    from .base_page import (
        BasePage, IllustrationArea, set_welcome_label_style, welcome_theme,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        BasePage, IllustrationArea, set_welcome_label_style, welcome_theme,
    )


_tr = make_tr("WelcomeWizard")

# 键位是否能作为全局快捷键的组成部分。放在模块级而不是类里：类体中的生成器
# 表达式看不到类作用域的名字。
BINDABLE = True
UNBOUND = False

# 键面上画的字 → 录入框实际写进文本的 token（全小写）。
# 绝大多数键两者一致，只有这两个画的是符号。
_LABEL_TOKEN_OVERRIDES = {
    "⌫": "backspace",
    "↵": "enter",
    "Caps": "capslock",
}


def _label_token(label: str) -> str:
    return _LABEL_TOKEN_OVERRIDES.get(label, label.lower())


def pressed_tokens(hotkey_text: str) -> frozenset:
    """把 "ctrl+shift+a" 这类录入结果拆成键面 token 集合。

    录入框在组合键输入到一半时写的是 "ctrl+"，split 后会留下空串，要滤掉。
    """
    return frozenset(
        part for part in (hotkey_text or "").strip().lower().split("+") if part
    )


# ── 插画：键盘 + 鼠标，标出哪些键能绑 ────────────────────
class _KeyboardMap(QWidget):
    """一张静态的可用性示意图：能绑的键高亮，不能绑的置灰。

    哪些键能绑不是随手涂的，而是照着 ShortcutManager._parse_hotkey 实际接受的
    范围来：字母、数字、F1–F24、Esc、以及各种标点都能作主键，Tab / Caps /
    Enter / Space / Backspace 解析不了。修改解析器时这张表要跟着改。
    """

    # 每行 (标签, 宽度单位, 是否可绑)。宽度以 1 个字母键为 1 单位。
    ROWS = (
        (
            ("Esc", 2, BINDABLE),
            *((f"F{n}", 1, BINDABLE) for n in range(1, 13)),
        ),
        (
            ("`", 1, BINDABLE),
            *((c, 1, BINDABLE) for c in "1234567890"),
            ("-", 1, BINDABLE), ("=", 1, BINDABLE),
            ("⌫", 2, UNBOUND),
        ),
        (
            ("Tab", 1.5, UNBOUND),
            *((c, 1, BINDABLE) for c in "QWERTYUIOP"),
            ("[", 1, BINDABLE), ("]", 1, BINDABLE), ("\\", 1.5, BINDABLE),
        ),
        (
            ("Caps", 1.75, UNBOUND),
            *((c, 1, BINDABLE) for c in "ASDFGHJKL"),
            (";", 1, BINDABLE), ("'", 1, BINDABLE),
            ("↵", 2.25, UNBOUND),
        ),
        (
            ("Shift", 2.25, BINDABLE),
            *((c, 1, BINDABLE) for c in "ZXCVBNM"),
            (",", 1, BINDABLE), (".", 1, BINDABLE), ("/", 1, BINDABLE),
            ("Shift", 2.75, BINDABLE),
        ),
        (
            ("Ctrl", 2, BINDABLE), ("Win", 1.5, BINDABLE), ("Alt", 1.5, BINDABLE),
            ("Space", 6.5, UNBOUND),
            ("Alt", 1.5, BINDABLE), ("Ctrl", 2, BINDABLE),
        ),
    )

    GRID_UNITS = 15          # 最宽的一行占多少单位
    KEY_GAP = 1.6
    ROW_GAP = 2.4

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(230)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self._pressed = frozenset()
        # 逐键缩字号的结果按 (键标, 键宽) 缓存：键盘是静态的，没必要每帧重算
        self._font_cache = {}

    def set_highlight(self, hotkey_text: str):
        """高亮某个热键用到的键；传空串表示不高亮。

        这里刻意跟着录入框的**文本**走而不是去追物理按键状态：键盘组合键、
        系统热键、鼠标侧键三条路最后都汇成同一串文本，跟着它就能一份逻辑覆盖
        三种输入，也不必自己维护「哪些键还按着」。
        """
        pressed = pressed_tokens(hotkey_text)
        if pressed == self._pressed:
            return
        self._pressed = pressed
        self.update()

    @staticmethod
    def _font(pixel_size: int, *, bold=False) -> QFont:
        """使用系统字体回退，保证中、日、韩文字不会变成方框。"""
        font = QFont()
        font.setPixelSize(pixel_size)
        font.setBold(bold)
        return font

    @staticmethod
    def _with_alpha(color, alpha: int) -> QColor:
        result = QColor(color)
        result.setAlpha(max(0, min(255, int(alpha))))
        return result

    @classmethod
    def _key_colors(cls, theme, bindable: bool, pressed: bool = False):
        """键面的三种状态配色：当前按下 / 可绑定 / 不可绑定。

        不用 accent_soft + panel_subtle 这对语义色：它们在浅色主题下几乎同色，
        整张图会糊成一片。改用带透明度的主题色，明暗两套主题下都能拉开对比。
        「按下」用实心主题色，和另外两种半透明的档位一眼分得开。
        """
        if pressed:
            return QColor(theme.accent), QColor(theme.accent), "#FFFFFF"
        if bindable:
            return (
                cls._with_alpha(theme.accent, 58),
                cls._with_alpha(theme.accent, 170),
                theme.text,
            )
        return (
            cls._with_alpha(theme.text_soft, 26),
            cls._with_alpha(theme.text_soft, 90),
            theme.text_soft,
        )

    def paintEvent(self, _event):
        theme = welcome_theme()
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        rect = QRectF(self.rect())
        unit = (rect.width() - self.KEY_GAP * (self.GRID_UNITS - 1)) / self.GRID_UNITS
        key_h = max(13.0, unit * 1.06)
        keyboard_h = len(self.ROWS) * key_h + (len(self.ROWS) - 1) * self.ROW_GAP
        mouse_h = 74.0
        legend_h = len(self._legend_layout(p, theme, rect.width() - 8))             * self.LEGEND_LINE_H
        block_h = keyboard_h + 22 + mouse_h + 8 + legend_h

        top = max(rect.top(), rect.center().y() - block_h / 2)
        self._draw_keyboard(p, theme, rect, top, unit, key_h)
        self._draw_mouse(p, theme, rect.center().x(), top + keyboard_h + 22)
        self._draw_legend(p, theme, rect, top + keyboard_h + 22 + mouse_h + 8)
        p.end()

    # ── 键盘 ────────────────────────────────────────────
    def _fitted_font(self, p, label: str, box: QRectF) -> QFont:
        """挑一个能把这个键标放进键面的字号。

        F12 比 Q 宽一倍，统一字号总有一个放不下；逐键量一次再缩最省事。
        """
        cache_key = (label, round(box.width()))
        font = self._font_cache.get(cache_key)
        if font is not None:
            return font

        size = max(6, int(box.height() * 0.44))
        font = self._font(size)
        available = box.width() - 3
        while size > 5:
            font.setPixelSize(size)
            p.setFont(font)
            if p.fontMetrics().horizontalAdvance(label) <= available:
                break
            size -= 1
        font = QFont(font)
        self._font_cache[cache_key] = font
        return font

    def _draw_keyboard(self, p, theme, rect, top, unit, key_h):
        for row_index, row in enumerate(self.ROWS):
            row_units = sum(width for _label, width, _ok in row)
            row_w = row_units * unit + self.KEY_GAP * (len(row) - 1)
            x = rect.center().x() - row_w / 2
            y = top + row_index * (key_h + self.ROW_GAP)

            for label, width, bindable in row:
                key_w = width * unit
                self._draw_key(
                    p, theme, QRectF(x, y, key_w, key_h), label, bindable,
                )
                x += key_w + self.KEY_GAP

    def _draw_key(self, p, theme, box: QRectF, label: str, bindable: bool):
        pressed = _label_token(label) in self._pressed
        fill, border, text = self._key_colors(theme, bindable, pressed)
        p.setBrush(fill)
        p.setPen(QPen(border, 1.6 if pressed else 1))
        p.drawRoundedRect(box, 2.5, 2.5)

        p.setFont(self._fitted_font(p, label, box))
        p.setPen(QColor(text))
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, label)

    # ── 鼠标 ────────────────────────────────────────────
    def _draw_mouse(self, p, theme, center_x: float, top: float):
        body = QRectF(center_x - 25, top, 50, 74)
        p.setBrush(QColor(theme.panel_subtle))
        p.setPen(QPen(QColor(theme.border), 1.2))
        p.drawRoundedRect(body, 24, 24)

        # 左右键分隔与滚轮：只是让它看起来像鼠标，本身不可绑
        p.setPen(QPen(QColor(theme.border), 1))
        p.drawLine(int(body.center().x()), int(body.top() + 6),
                   int(body.center().x()), int(body.top() + 27))
        wheel = QRectF(body.center().x() - 3, body.top() + 10, 6, 13)
        p.setBrush(QColor(theme.border))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(wheel, 3, 3)

        # 侧键是可绑的那两个，按键盘同一套配色标出来
        # 上面那颗离鼠标前端更近，是「前进」；下面那颗是「后退」
        for index, token in enumerate(("mouseforward", "mouseback")):
            pressed = token in self._pressed
            fill, border, _text = self._key_colors(theme, True, pressed)
            side = QRectF(body.left() - 6, body.top() + 26 + index * 17, 10, 14)
            p.setBrush(fill)
            p.setPen(QPen(border, 1.6 if pressed else 1.2))
            p.drawRoundedRect(side, 3, 3)

        # 标签放在侧键这一侧并右对齐：写在鼠标右边会让人以为指的是右键
        p.setFont(self._font(10))
        p.setPen(QColor(theme.text_muted))
        label_right = body.left() - 12
        p.drawText(
            QRectF(label_right - 110, body.top() + 26, 110, 20),
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
            _tr("鼠标侧键"),
        )

    # ── 图例 ────────────────────────────────────────────
    LEGEND_SWATCH_W = 14
    LEGEND_TEXT_GAP = 5
    LEGEND_ITEM_GAP = 14
    LEGEND_LINE_H = 16

    def _legend_entries(self, theme):
        bindable_fill, bindable_border, _ = self._key_colors(theme, True)
        unbound_fill, unbound_border, _ = self._key_colors(theme, False)
        entries = [
            (bindable_fill, bindable_border, theme.text_muted, _tr("可设为快捷键")),
            (unbound_fill, unbound_border, theme.text_soft, _tr("不可用")),
        ]
        if self._pressed:
            # 没按键时不占这一格：没出现过的颜色先解释也没意义
            pressed_fill, pressed_border, _ = self._key_colors(theme, True, True)
            entries.append(
                (pressed_fill, pressed_border, theme.text_muted, _tr("当前按下"))
            )
        return entries

    def _legend_layout(self, p, theme, available_w: float):
        """把图例条目按可用宽度分行。

        四种语言的译文长度差很多，硬排一行在窄面板下会画到框外；自动换行比
        逐语言调字号省事，也不会有人看不见其中一档。
        """
        p.setFont(self._font(10))
        metrics = p.fontMetrics()
        lines, current, current_w = [], [], 0.0
        for entry in self._legend_entries(theme):
            width = (
                self.LEGEND_SWATCH_W + self.LEGEND_TEXT_GAP
                + metrics.horizontalAdvance(entry[3])
            )
            extra = width if not current else width + self.LEGEND_ITEM_GAP
            if current and current_w + extra > available_w:
                lines.append((current, current_w))
                current, current_w = [(entry, width)], width
                continue
            current.append((entry, width))
            current_w += extra
        if current:
            lines.append((current, current_w))
        return lines

    def _draw_legend(self, p, theme, rect, top: float):
        for line_index, (items, line_w) in enumerate(
            self._legend_layout(p, theme, rect.width() - 8)
        ):
            x = max(rect.left() + 4, rect.center().x() - line_w / 2)
            y = top + line_index * self.LEGEND_LINE_H

            for (fill, border, text_color, text), width in items:
                swatch = QRectF(x, y + 3, self.LEGEND_SWATCH_W, 10)
                p.setBrush(QColor(fill))
                p.setPen(QPen(QColor(border), 1))
                p.drawRoundedRect(swatch, 2, 2)

                p.setPen(QColor(text_color))
                p.drawText(
                    QRectF(
                        x + self.LEGEND_SWATCH_W + self.LEGEND_TEXT_GAP,
                        y, width, self.LEGEND_LINE_H,
                    ),
                    Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                    text,
                )
                x += width + self.LEGEND_ITEM_GAP


class _HotkeyIllus(IllustrationArea):
    def _build_content(self):
        self._layout.setContentsMargins(10, 10, 10, 10)
        self.keyboard_map = _KeyboardMap(self)
        self._layout.addWidget(self.keyboard_map, 1)

    def retranslate(self):
        self.keyboard_map.update()

    def _apply_welcome_theme(self, tokens=None):
        super()._apply_welcome_theme(tokens)
        if hasattr(self, "keyboard_map"):
            self.keyboard_map.update()


# ── 页面主体 ────────────────────────────────────────────
class HotkeyPage(BasePage):
    """第2页：六个全局快捷键"""

    # (主键读, 主键写, 备用读, 备用写, 标题)。顺序即界面上从上到下的顺序。
    ROWS = (
        ("get_hotkey", "set_hotkey", "get_hotkey_2", "set_hotkey_2", "截图"),
        (
            "get_clipboard_hotkey", "set_clipboard_hotkey",
            "get_clipboard_hotkey_2", "set_clipboard_hotkey_2", "剪贴板",
        ),
        (
            "get_translation_hotkey", "set_translation_hotkey",
            "get_translation_hotkey_2", "set_translation_hotkey_2", "智能翻译",
        ),
    )

    _SLOT_LABEL_W = 62

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title=_tr("快捷键"),
            subtitle=_tr("三个全局功能各可设置一个主快捷键和一个备用快捷键。"),
            parent=parent,
        )

    def _create_illustration(self):
        return _HotkeyIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        if __package__:
            from ..hotkey_edit import HotkeyEdit, validate_hotkey_group
        else:
            import sys, os
            sys.path.insert(
                0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
            )
            from hotkey_edit import HotkeyEdit, validate_hotkey_group

        self._validate = validate_hotkey_group
        self._section_labels = []
        self._slot_labels = []
        self._edits = []
        self._row_edits = []

        # 三个功能各占一块、标题单独一行。之前是三列网格，两个录入框挤在标题
        # 右边分不到宽度，状态图标会压到输入框上。
        for index, (getter, _setter, getter_2, _setter_2, title) in enumerate(
            self.ROWS
        ):
            if index:
                layout.addSpacing(14)

            section_lbl = QLabel(_tr(title))
            set_welcome_label_style(
                section_lbl, role="primary", font_size=14, weight=600
            )
            layout.addWidget(section_lbl)
            self._section_labels.append((section_lbl, title))
            layout.addSpacing(6)

            pair = []
            for slot_title, accessor in (("主快捷键", getter), ("备用", getter_2)):
                edit = HotkeyEdit()
                edit.setText(self._read(accessor))
                edit.setSizePolicy(
                    QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed
                )

                slot_lbl = QLabel(_tr(slot_title))
                slot_lbl.setFixedWidth(self._SLOT_LABEL_W)
                set_welcome_label_style(
                    slot_lbl, role="muted", font_size=12, weight=400
                )
                self._slot_labels.append((slot_lbl, slot_title))

                row = QHBoxLayout()
                row.setContentsMargins(0, 0, 0, 0)
                row.setSpacing(8)
                row.addWidget(slot_lbl)
                row.addWidget(edit, 1)
                layout.addLayout(row)
                if slot_title == "主快捷键":
                    layout.addSpacing(6)

                self._edits.append(edit)
                pair.append(edit)
            self._row_edits.append(tuple(pair))

        layout.addSpacing(14)
        self._hint = QLabel(
            _tr("点击输入框后，按下快捷键或鼠标侧键即可录入。")
        )
        self._hint.setWordWrap(True)
        set_welcome_label_style(self._hint, role="muted", font_size=12, weight=400)
        layout.addWidget(self._hint)

        # 判重连接放在六个框全部创建之后，避免初始化途中只看到半组控件；
        # 最后主动跑一次，以识别配置文件里遗留的旧冲突。
        for edit in self._edits:
            edit.textChanged.connect(self._on_edit_text_changed)
        self._revalidate()

        # 左侧插画跟着「当前正在编辑的那一格」高亮。用全局 focusChanged 而不是
        # 给 HotkeyEdit 加信号：这是本页自己的联动，没必要让共用控件为它加接口。
        self._active_edit = None
        self._line_to_edit = {edit.edit: edit for edit in self._edits}
        app = QApplication.instance()
        if app is not None:
            app.focusChanged.connect(self._on_focus_changed)

    def _on_focus_changed(self, _old, now):
        self._active_edit = self._line_to_edit.get(now)
        self._sync_key_highlight()

    def _on_edit_text_changed(self, *_args):
        self._revalidate()
        self._sync_key_highlight()

    def _sync_key_highlight(self):
        keyboard = getattr(self.illus_area, "keyboard_map", None)
        if keyboard is None:
            return
        edit = self._active_edit
        keyboard.set_highlight(edit.text() if edit is not None else "")

    def _read(self, accessor: str) -> str:
        getter = getattr(self._config, accessor, None)
        if getter is None:
            return ""
        try:
            return getter() or ""
        except Exception as e:
            log_exception(e, T("读取全局快捷键 {accessor}", accessor=accessor))
            return ""

    def _revalidate(self, *_args) -> bool:
        return self._validate(self._edits)

    def retranslate(self):
        self.title_label.setText(_tr("快捷键"))
        self.subtitle_label.setText(
            _tr("三个全局功能各可设置一个主快捷键和一个备用快捷键。")
        )
        for label, text in getattr(self, "_section_labels", ()):
            label.setText(_tr(text))
        for label, text in getattr(self, "_slot_labels", ()):
            label.setText(_tr(text))
        if hasattr(self, "_hint"):
            self._hint.setText(
                _tr("点击输入框后，按下快捷键或鼠标侧键即可录入。")
            )
        # 冲突提示文案本身也要跟着语言走
        self._revalidate()
        if hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    def save(self):
        """写回六个全局快捷键，跳过仍在冲突中的值。

        向导的「跳过」和关闭按钮同样会走到这里，所以冲突不能只在「下一步」
        上拦——冲突值一旦落盘，对应的热键注册必定静默失败。冲突的那一格保留
        原有配置，其余照常保存。
        """
        conflicted = not self._revalidate()
        for (getter, setter, getter_2, setter_2, _title), (edit, edit_2) in zip(
            self.ROWS, self._row_edits
        ):
            self._write(setter, edit, conflicted, keep_when_blank=True)
            self._write(setter_2, edit_2, conflicted, keep_when_blank=False)

    def _write(self, accessor: str, edit, conflicted: bool, *, keep_when_blank: bool):
        if conflicted and edit.validation_error:
            return
        setter = getattr(self._config, accessor, None)
        if setter is None:
            return
        value = edit.text().strip()
        if keep_when_blank and not value:
            # 主快捷键留空等于没有任何入口，保留原值更安全；备用键留空是正常选择。
            return
        try:
            setter(value)
        except Exception as e:
            log_exception(e, T("保存全局快捷键 {accessor}", accessor=accessor))


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._go_to_page(1)
    w.show()
    sys.exit(app.exec())
