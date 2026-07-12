# -*- coding: utf-8 -*-
"""
提交范围 77eee29..HEAD 的集成验证测试。

验证该范围内新增功能在源代码中的存在性和基本正确性：
- 序号工具重构（NumberTool 新方法、NumberEditCommand 等）
- 文字选择开关（PinOCRManager/PinWindow/PinContextMenu）
- 全局热键禁用（ShortcutManager/HotkeySystem/MainApp）
- PDF 保存格式（SaveService）
- CanvasView/SmartEditController cleanup 安全性
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QPointF
from PySide6.QtGui import QColor


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


# ============================================================================
# 1. 序号工具重构验证
# ============================================================================

class TestNumberToolRefactor:
    """验证 NumberTool 新增的类方法"""

    def test_get_max_number_exists(self):
        from tools.number import NumberTool
        assert hasattr(NumberTool, "get_max_number")
        assert callable(NumberTool.get_max_number)

    def test_assign_number_order_exists(self):
        from tools.number import NumberTool
        assert hasattr(NumberTool, "assign_number_order")
        assert callable(NumberTool.assign_number_order)

    def test_get_next_after_number_edit_exists(self):
        from tools.number import NumberTool
        assert hasattr(NumberTool, "get_next_after_number_edit")
        assert callable(NumberTool.get_next_after_number_edit)

    def test_set_next_number_and_refresh_exists(self):
        from tools.number import NumberTool
        assert hasattr(NumberTool, "set_next_number_and_refresh")
        assert callable(NumberTool.set_next_number_and_refresh)

    def test_refresh_next_number_exists(self):
        from tools.number import NumberTool
        assert hasattr(NumberTool, "refresh_next_number")
        assert callable(NumberTool.refresh_next_number)

    def test_scene_order_attr_defined(self):
        from tools.number import NumberTool
        assert NumberTool._SCENE_ORDER_ATTR == "_number_tool_order_counter"


class TestNumberUndoCommands:
    """验证新增的 Number 撤销命令类"""

    def test_add_number_command_exists(self):
        from canvas.undo import AddNumberCommand
        assert issubclass(AddNumberCommand, object.__class__.__base__ or type)

    def test_remove_number_command_exists(self):
        from canvas.undo import RemoveNumberCommand
        assert hasattr(RemoveNumberCommand, "undo")
        assert hasattr(RemoveNumberCommand, "redo")

    def test_remove_number_and_renumber_command_exists(self):
        from canvas.undo import RemoveNumberAndRenumberCommand
        assert hasattr(RemoveNumberAndRenumberCommand, "_build_new_numbers")

    def test_number_edit_command_exists(self):
        from canvas.undo import NumberEditCommand
        assert hasattr(NumberEditCommand, "mergeWith")
        assert NumberEditCommand.MERGE_WINDOW_SECONDS == 0.7
        assert NumberEditCommand.COMMAND_ID == 0x4E554D

    def test_remove_number_item_command_alias(self):
        from canvas.undo import RemoveNumberItemCommand, RemoveNumberCommand
        assert RemoveNumberItemCommand is RemoveNumberCommand

    def test_number_support_in_edit_item_command(self):
        """EditItemCommand 应支持 number 属性恢复"""
        from canvas.undo import EditItemCommand
        from canvas.items import NumberItem
        item = NumberItem(5, QPointF(0, 0), 20, QColor(255, 0, 0))
        cmd = EditItemCommand(item, {"number": 5}, {"number": 10})
        cmd.redo()
        assert item.number == 10
        cmd.undo()
        assert item.number == 5


class TestNumberItemChanges:
    """验证 NumberItem 新增的属性和方法"""

    def test_number_order_attribute(self):
        from canvas.items import NumberItem
        item = NumberItem(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        assert hasattr(item, "number_order")
        assert item.number_order is None

    def test_visual_rect_method(self):
        from canvas.items import NumberItem
        item = NumberItem(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        assert hasattr(item, "visualRect")
        assert callable(item.visualRect)

    def test_scene_visual_rect_method(self):
        from canvas.items import NumberItem
        item = NumberItem(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        assert hasattr(item, "sceneVisualRect")
        assert callable(item.sceneVisualRect)

    def test_shape_method_uses_click_margin(self):
        from canvas.items import NumberItem
        item = NumberItem(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        assert hasattr(item, "CLICK_MARGIN")
        assert item.CLICK_MARGIN == 6


# ============================================================================
# 2. 文字选择开关验证
# ============================================================================

class TestTextSelectionFeature:
    """验证 PinOCRManager 文字选择开关"""

    def test_ocr_manager_has_text_selection_enabled(self):
        from unittest.mock import MagicMock
        from pin.pin_ocr_manager import PinOCRManager
        win = MagicMock()
        cfg = MagicMock()
        mgr = PinOCRManager(win, cfg)
        assert hasattr(mgr, "_text_selection_enabled")
        assert mgr._text_selection_enabled is True
        assert hasattr(mgr, "text_selection_enabled")

    def test_ocr_manager_has_apply_text_layer_enabled(self):
        from unittest.mock import MagicMock
        from pin.pin_ocr_manager import PinOCRManager
        win = MagicMock()
        cfg = MagicMock()
        mgr = PinOCRManager(win, cfg)
        assert hasattr(mgr, "_apply_text_layer_enabled")
        assert callable(mgr._apply_text_layer_enabled)

    def test_ocr_manager_has_set_text_selection_enabled(self):
        from unittest.mock import MagicMock
        from pin.pin_ocr_manager import PinOCRManager
        win = MagicMock()
        cfg = MagicMock()
        mgr = PinOCRManager(win, cfg)
        assert hasattr(mgr, "set_text_selection_enabled")
        assert callable(mgr.set_text_selection_enabled)

    def test_ocr_manager_has_toggle_text_selection(self):
        from unittest.mock import MagicMock
        from pin.pin_ocr_manager import PinOCRManager
        win = MagicMock()
        cfg = MagicMock()
        mgr = PinOCRManager(win, cfg)
        assert hasattr(mgr, "toggle_text_selection")
        assert callable(mgr.toggle_text_selection)
        # toggle 应返回 bool
        result = mgr.toggle_text_selection()
        assert isinstance(result, bool)

    def test_ocr_manager_temporarily_enabled(self):
        from unittest.mock import MagicMock
        from pin.pin_ocr_manager import PinOCRManager
        win = MagicMock()
        cfg = MagicMock()
        mgr = PinOCRManager(win, cfg)
        assert hasattr(mgr, "_temporary_enabled")
        assert mgr._temporary_enabled is True

    def test_context_menu_state_has_text_selection(self):
        """pin_context_menu 应支持 text_selection_enabled 状态键"""
        # 验证 PinWindow._context_menu_state 返回的字典包含该键
        # （需要实际 PinWindow 实例，这里仅验证函数引用存在）
        from pin.pin_window import PinWindow
        assert hasattr(PinWindow, "toggle_text_selection")


# ============================================================================
# 3. 全局热键禁用验证
# ============================================================================

class TestGlobalHotkeySuppression:
    """验证全局热键禁用功能"""

    def test_shortcut_manager_has_suppressed_property(self):
        from core.shortcut_manager import ShortcutManager
        mgr = ShortcutManager()
        assert hasattr(mgr, "global_hotkeys_suppressed")
        assert mgr.global_hotkeys_suppressed is False

    def test_shortcut_manager_set_suppressed(self):
        from core.shortcut_manager import ShortcutManager
        mgr = ShortcutManager()
        mgr.set_global_hotkeys_suppressed(True)
        assert mgr.global_hotkeys_suppressed is True
        mgr.set_global_hotkeys_suppressed(False)
        assert mgr.global_hotkeys_suppressed is False

    def test_shortcut_manager_has_registered_hotkeys(self):
        from core.shortcut_manager import ShortcutManager
        mgr = ShortcutManager()
        assert hasattr(mgr, "has_registered_hotkeys")
        assert mgr.has_registered_hotkeys() is False  # 初始无注册

    def test_hotkey_system_set_suppressed(self):
        from core.shortcut_manager import HotkeySystem
        hs = HotkeySystem()
        assert hasattr(hs, "set_suppressed")
        assert hasattr(hs, "has_registered_hotkeys")
        # 不崩溃即可
        hs.set_suppressed(True)
        hs.set_suppressed(False)

    def test_default_setting_global_hotkeys_disabled(self):
        from settings.tool_settings import ToolSettingsManager
        defaults = ToolSettingsManager.APP_DEFAULT_SETTINGS
        assert "global_hotkeys_disabled" in defaults
        assert defaults["global_hotkeys_disabled"] is False


# ============================================================================
# 4. PDF 保存格式验证
# ============================================================================

class TestPDFSaveFormat:
    """验证 PDF 保存功能"""

    def test_pdf_in_save_format_options(self):
        from settings.tool_settings import ToolSettingsManager
        comment = ToolSettingsManager.set_screenshot_format.__doc__ or ""
        assert "PDF" in comment

    def test_save_service_has_pdf_methods(self):
        from core.save import SaveService
        svc = SaveService()
        assert hasattr(svc, "_save_qimage_to_pdf_path")
        assert hasattr(svc, "_flatten_for_pdf")
        assert hasattr(svc, "_normalize_format")
        assert hasattr(svc, "save_qimage_to_path")

    def test_default_pdf_dpi(self):
        from core.save import SaveService
        assert SaveService.DEFAULT_PDF_DPI == 300


# ============================================================================
# 5. Cleanup 安全性验证
# ============================================================================

class TestCleanupSafety:
    """验证 CanvasView/SmartEditController cleanup 方法"""

    def test_canvas_view_has_cleanup(self):
        from canvas.view import CanvasView
        assert hasattr(CanvasView, "cleanup")
        assert callable(getattr(CanvasView, "cleanup"))

    def test_smart_edit_controller_has_cleanup(self):
        from canvas.smart_edit_controller import SmartEditController
        assert hasattr(SmartEditController, "cleanup")
        assert callable(getattr(SmartEditController, "cleanup"))

    def test_tool_controller_has_remove_callback(self):
        from tools.controller import ToolController
        from unittest.mock import MagicMock
        tc = ToolController(MagicMock())
        assert hasattr(tc, "remove_tool_changed_callback")
        assert callable(tc.remove_tool_changed_callback)

    def test_closed_view_skips_signals(self, qapp):
        """_is_closed 守卫应正常工作"""
        from PySide6.QtCore import QRectF
        from PySide6.QtGui import QImage
        from canvas.scene import CanvasScene
        from canvas.view import CanvasView
        bg = QImage(100, 100, QImage.Format.Format_ARGB32)
        bg.fill(0xFFFFFFFF)
        scene = CanvasScene(bg, QRectF(0, 0, 100, 100))
        view = CanvasView(scene)
        view._is_closed = True
        # 不应崩溃
        view._on_cursor_tool_update_requested("pen", True)
        view._on_item_auto_select_requested(None)
        view.cleanup()  # 二次 cleanup 应为空操作


# ============================================================================
# 6. NumberItem 渲染变更验证
# ============================================================================

class TestNumberHandleEditor:
    """验证 LayerEditor 序号手柄变更"""

    def test_number_handle_types_exist(self):
        from canvas.handle_editor import HandleType
        assert HandleType.NUMBER_INCREMENT.value == "number_increment"
        assert HandleType.NUMBER_DECREMENT.value == "number_decrement"
        assert HandleType.NUMBER_DELETE.value == "number_delete"

    def test_layer_editor_has_number_constants(self):
        from canvas.handle_editor import LayerEditor
        assert hasattr(LayerEditor, "NUMBER_BUTTON_SIZE")
        assert hasattr(LayerEditor, "NUMBER_BUTTON_GAP")
        assert hasattr(LayerEditor, "NUMBER_BUTTON_COLOR")
        assert hasattr(LayerEditor, "NUMBER_BUTTON_HOVER_COLOR")

    def test_is_number_adjust_handle_exists(self):
        from canvas.handle_editor import LayerEditor
        assert hasattr(LayerEditor, "is_number_adjust_handle")
        assert hasattr(LayerEditor, "is_number_delete_handle")
        assert hasattr(LayerEditor, "adjust_number_with_handle")

    def test_generate_number_handles_returns_three(self, qapp):
        from PySide6.QtCore import QRectF
        from canvas.handle_editor import LayerEditor, HandleType
        editor = LayerEditor()
        rect = QRectF(10, 10, 40, 40)
        handles = editor._generate_number_handles(rect)
        assert len(handles) == 3
        types = {h.handle_type for h in handles}
        assert HandleType.NUMBER_INCREMENT in types
        assert HandleType.NUMBER_DECREMENT in types
        assert HandleType.NUMBER_DELETE in types
        # + 左上角，X 右上角（对称），- 在 + 正下方
        h_inc = next(h for h in handles if h.handle_type == HandleType.NUMBER_INCREMENT)
        h_dec = next(h for h in handles if h.handle_type == HandleType.NUMBER_DECREMENT)
        h_del = next(h for h in handles if h.handle_type == HandleType.NUMBER_DELETE)
        assert h_inc.position.x() == rect.left()
        assert h_inc.position.y() == rect.top()
        assert h_del.position.x() == rect.right()
        assert h_del.position.y() == rect.top()
        assert h_dec.position.x() == rect.left()
        assert h_dec.position.y() == rect.top() + editor.NUMBER_BUTTON_SIZE + editor.NUMBER_BUTTON_GAP

