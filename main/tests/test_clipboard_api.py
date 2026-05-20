# -*- coding: utf-8 -*-

from clipboard import (
    ClipboardItem,
    ClipboardManager,
    ClipboardWindow,
    Group,
    GroupType,
    ManageDialog,
    get_existing_manage_dialog,
    get_manage_dialog,
)
from clipboard.controllers import ClipboardController as ControllersClipboardController, SelectionManager as ControllersSelectionManager
from clipboard.controllers.clipboard_controller import ClipboardController as ModuleClipboardController
from clipboard.controllers.selection_manager import SelectionManager as ModuleSelectionManager
from clipboard.core import (
    ClipboardItem as CoreClipboardItem,
    ClipboardManager as CoreClipboardManager,
    Group as CoreGroup,
    GroupType as CoreGroupType,
)
from clipboard.core.manager import ClipboardManager as ModuleCoreClipboardManager
from clipboard.ui.dialogs.manage_dialog import (
    ManageDialog as UiManageDialog,
    get_existing_manage_dialog as ui_get_existing_manage_dialog,
    get_manage_dialog as ui_get_manage_dialog,
)
from clipboard.ui.windows.clipboard_window import ClipboardWindow as UiClipboardWindow


def test_clipboard_package_exports_match_real_modules():
    assert ClipboardItem is CoreClipboardItem
    assert ClipboardManager is CoreClipboardManager
    assert ClipboardWindow is UiClipboardWindow
    assert Group is CoreGroup
    assert GroupType is CoreGroupType
    assert ManageDialog is UiManageDialog
    assert get_existing_manage_dialog is ui_get_existing_manage_dialog
    assert get_manage_dialog is ui_get_manage_dialog


def test_core_manager_entrypoints_match_module_path():
    assert ClipboardManager is ModuleCoreClipboardManager


def test_controller_entrypoints_match_module_paths():
    assert ControllersClipboardController is ModuleClipboardController
    assert ControllersSelectionManager is ModuleSelectionManager