# -*- coding: utf-8 -*-
"""
钉图管理器测试（pin/pin_manager.py）

PinManager 是 168 条语句、覆盖率 25% 的单例，负责跟踪所有贴图窗口并做批量操作。
它有两处特别容易出错又没人测的地方：一是单例本身的建立方式（__new__ 与
__init__ 的重入保护是分开写的），二是截图时把贴图窗口的置顶状态临时降级、
截完再恢复的那对 suppress/restore —— 后者用 Win32 SetWindowPos 直接操作，
一旦状态位算错，用户会看到贴图永久失去置顶，或者截图时贴图挡在取景框上。

隔离方式：不构造真实 PinWindow（那会拉起画布、OCR、快捷键单例并强制 show），
而是用只实现被调用到的那几个方法的假窗口。Win32 调用通过替换模块级 _user32
拦下来，因此测试不会真的动任何窗口。

单例状态是跨用例共享的，每个用例前后都把 PinManager._instance 清成 None，
否则前一个用例注册的假窗口会漏进后一个用例。
"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QRect, Qt

from pin.pin_manager import PinManager, get_pin_manager

TOPMOST = Qt.WindowType.WindowStaysOnTopHint
PLAIN = Qt.WindowType.FramelessWindowHint

# 源码里的 Win32 常量，测试断言直接引用避免抄错
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_FLAGS = 0x0002 | 0x0001 | 0x0010


@pytest.fixture(autouse=True)
def silence_logging(monkeypatch):
    """日志是模块顶部 import 的，静音掉避免真的落盘"""
    for name in ("log_info", "log_debug", "log_error"):
        monkeypatch.setattr(f"pin.pin_manager.{name}", lambda *a, **k: None)


@pytest.fixture(autouse=True)
def reset_singleton():
    PinManager._instance = None
    yield
    PinManager._instance = None


@pytest.fixture
def manager(qapp, reset_singleton):
    """QObject 需要 QApplication，因此依赖 conftest 的 session 级 qapp"""
    return PinManager.instance()


@pytest.fixture
def fake_user32(monkeypatch):
    class _FakeUser32:
        def __init__(self):
            self.calls = []

        def SetWindowPos(self, *args):
            self.calls.append(args)
            return True

    fake = _FakeUser32()
    monkeypatch.setattr("pin.pin_manager._user32", fake)
    return fake


class _FakePin:
    """
    只实现 PinManager 会碰到的接口。

    刻意不用 MagicMock：save_all_to_directory 用 hasattr 判断
    _with_edit_paused 是否存在，MagicMock 会让这个分支恒为真。
    """

    def __init__(self, flags=TOPMOST, hwnd=1000, thumbnail=False,
                 save_ok=True, save_raises=None, with_edit_paused=False):
        self._flags = flags
        self._hwnd = hwnd
        self._thumbnail_mode = thumbnail
        self._save_ok = save_ok
        self._save_raises = save_raises
        self.closed_count = 0
        self.shown = 0
        self.hidden = 0
        self.toggled = 0
        self.moved_to = []
        self.saved_paths = []
        self.edit_paused_calls = 0
        if with_edit_paused:
            self._with_edit_paused = self._pause_edit

    # ── 置顶相关 ──
    def windowFlags(self):
        return self._flags

    def winId(self):
        return self._hwnd

    # ── 批量操作 ──
    def close_window(self):
        self.closed_count += 1

    def show(self):
        self.shown += 1

    def hide(self):
        self.hidden += 1

    def toggle_thumbnail_mode(self):
        self.toggled += 1
        self._thumbnail_mode = not self._thumbnail_mode

    def geometry(self):
        return QRect(0, 0, 200, 100)

    def width(self):
        return 200

    def height(self):
        return 100

    def move(self, x, y):
        self.moved_to.append((x, y))

    # ── 保存 ──
    def get_current_image(self):
        pin = self

        class _Image:
            def save(self, path):
                if pin._save_raises is not None:
                    raise pin._save_raises
                pin.saved_paths.append(path)
                return pin._save_ok

        return _Image()

    def _pause_edit(self, func):
        self.edit_paused_calls += 1
        func()


class _RaisingPin(_FakePin):
    def close_window(self):
        raise RuntimeError("窗口已被销毁")


# ============================================================================
# 单例
# ============================================================================

class TestSingleton:

    def test_instance_returns_the_same_object_every_time(self, qapp):
        first = PinManager.instance()
        assert PinManager.instance() is first
        assert PinManager.get_instance() is first
        assert get_pin_manager() is first

    def test_constructor_returns_the_established_instance(self, qapp):
        established = PinManager.instance()
        assert PinManager() is established

    def test_reinitialisation_does_not_wipe_tracked_windows(self, qapp):
        """
        __init__ 用 '_initialized' in self.__dict__ 做重入保护。
        如果它失效，第二次构造会把 pin_windows 清成空列表，
        管理器就此丢失所有已跟踪的窗口而没有任何报错。
        """
        manager = PinManager.instance()
        manager.pin_windows.append(_FakePin())
        PinManager()
        assert manager.count() == 1

    def test_direct_construction_does_not_establish_the_singleton(self, qapp):
        """
        已知行为：__new__ 只读 _instance 不写它，写入只发生在 instance()。
        因此绕过 instance() 直接构造会得到两个互不相关的管理器——
        这正是必须统一走 instance() / get_pin_manager() 的原因。
        """
        first = PinManager()
        second = PinManager()
        assert first is not second
        assert PinManager._instance is None

    def test_cleanup_clears_the_singleton_so_the_next_call_rebuilds(self, qapp):
        manager = PinManager.instance()
        manager.cleanup()
        assert PinManager._instance is None
        assert PinManager.instance() is not manager


# ============================================================================
# 跟踪与批量操作
# ============================================================================

class TestTracking:

    def test_a_fresh_manager_tracks_nothing(self, manager):
        assert manager.count() == 0
        assert manager.has_pins() is False
        assert manager.get_all_pins() == []

    def test_get_all_pins_returns_a_copy(self, manager):
        """调用方拿到的列表被改动不应影响管理器自己的跟踪表"""
        manager.pin_windows.append(_FakePin())
        snapshot = manager.get_all_pins()
        snapshot.clear()
        assert manager.count() == 1

    def test_remove_pin_stops_tracking_without_closing_the_window(self, manager):
        pin = _FakePin()
        manager.pin_windows.append(pin)
        manager.remove_pin(pin)
        assert manager.count() == 0
        assert pin.closed_count == 0

    def test_removing_an_untracked_window_is_a_no_op(self, manager):
        manager.pin_windows.append(_FakePin())
        manager.remove_pin(_FakePin())
        assert manager.count() == 1

    def test_show_all_and_hide_all_reach_every_window(self, manager):
        pins = [_FakePin(), _FakePin()]
        manager.pin_windows.extend(pins)
        manager.show_all()
        manager.hide_all()
        for pin in pins:
            assert pin.shown == 1
            assert pin.hidden == 1


class TestCloseNotifications:

    def test_closing_one_of_several_only_emits_pin_closed(self, manager):
        closed, all_closed = [], []
        manager.pin_closed.connect(closed.append)
        manager.all_pins_closed.connect(lambda: all_closed.append(True))

        first, second = _FakePin(), _FakePin()
        manager.pin_windows.extend([first, second])
        manager._on_pin_closed(first)

        assert closed == [first]
        assert all_closed == []
        assert manager.count() == 1

    def test_closing_the_last_window_also_emits_all_pins_closed(self, manager):
        all_closed = []
        manager.all_pins_closed.connect(lambda: all_closed.append(True))
        pin = _FakePin()
        manager.pin_windows.append(pin)
        manager._on_pin_closed(pin)
        assert all_closed == [True]

    def test_a_duplicate_close_callback_is_ignored(self, manager):
        """窗口可能同时被手动关闭和信号回调触发，第二次必须安静跳过"""
        all_closed = []
        manager.all_pins_closed.connect(lambda: all_closed.append(True))
        pin = _FakePin()
        manager.pin_windows.append(pin)
        manager._on_pin_closed(pin)
        manager._on_pin_closed(pin)
        assert all_closed == [True]


class TestCloseAll:

    def test_close_all_closes_every_window_and_clears_the_list(self, manager):
        pins = [_FakePin(), _FakePin(), _FakePin()]
        manager.pin_windows.extend(pins)
        manager.close_all()
        assert all(pin.closed_count == 1 for pin in pins)
        assert manager.count() == 0

    def test_close_all_emits_all_pins_closed_once(self, manager):
        all_closed = []
        manager.all_pins_closed.connect(lambda: all_closed.append(True))
        manager.pin_windows.extend([_FakePin(), _FakePin()])
        manager.close_all()
        assert all_closed == [True]

    def test_close_all_on_an_empty_manager_stays_silent(self, manager):
        """没有窗口时提前 return，不该发出"全部已关闭"的信号"""
        all_closed = []
        manager.all_pins_closed.connect(lambda: all_closed.append(True))
        manager.close_all()
        assert all_closed == []

    def test_one_failing_window_does_not_block_the_others(self, manager):
        healthy_before, broken, healthy_after = _FakePin(), _RaisingPin(), _FakePin()
        manager.pin_windows.extend([healthy_before, broken, healthy_after])
        manager.close_all()
        assert healthy_before.closed_count == 1
        assert healthy_after.closed_count == 1
        assert manager.count() == 0


class TestThumbnailBatch:

    def test_only_windows_in_the_wrong_state_are_toggled(self, manager):
        already_thumbnail = _FakePin(thumbnail=True)
        normal = _FakePin(thumbnail=False)
        manager.pin_windows.extend([already_thumbnail, normal])
        manager.set_all_thumbnail_mode(True)
        assert already_thumbnail.toggled == 0
        assert normal.toggled == 1

    def test_the_batch_call_is_idempotent(self, manager):
        pin = _FakePin(thumbnail=False)
        manager.pin_windows.append(pin)
        manager.set_all_thumbnail_mode(True)
        manager.set_all_thumbnail_mode(True)
        assert pin.toggled == 1

    def test_leaving_thumbnail_mode_toggles_only_the_active_ones(self, manager):
        active, inactive = _FakePin(thumbnail=True), _FakePin(thumbnail=False)
        manager.pin_windows.extend([active, inactive])
        manager.set_all_thumbnail_mode(False)
        assert active.toggled == 1
        assert inactive.toggled == 0


class TestMoveToScreenCenter:

    def _patch_screen(self, monkeypatch, available, at_result="use-primary"):
        primary = SimpleNamespace(availableGeometry=lambda: available)
        target = primary if at_result == "use-primary" else at_result

        class _FakeQApplication:
            @staticmethod
            def screenAt(point):
                return None if at_result == "use-primary" else target

            @staticmethod
            def primaryScreen():
                return primary

        monkeypatch.setattr("pin.pin_manager.QApplication", _FakeQApplication)

    def test_window_is_centred_inside_the_available_area(self, manager, monkeypatch):
        self._patch_screen(monkeypatch, QRect(0, 0, 1920, 1080))
        pin = _FakePin()  # 200x100
        manager.pin_windows.append(pin)
        manager.move_all_to_screen_center()
        assert pin.moved_to == [((1920 - 200) // 2, (1080 - 100) // 2)]

    def test_secondary_screen_origin_is_added_to_the_offset(self, manager, monkeypatch):
        """副屏的 availableGeometry 原点不是 0，居中必须带上它"""
        self._patch_screen(monkeypatch, QRect(1920, 40, 1280, 1000))
        pin = _FakePin()
        manager.pin_windows.append(pin)
        manager.move_all_to_screen_center()
        assert pin.moved_to == [(1920 + (1280 - 200) // 2, 40 + (1000 - 100) // 2)]

    def test_a_failing_window_does_not_stop_the_rest(self, manager, monkeypatch):
        self._patch_screen(monkeypatch, QRect(0, 0, 1920, 1080))

        class _BrokenPin(_FakePin):
            def geometry(self):
                raise RuntimeError("窗口已被销毁")

        broken, healthy = _BrokenPin(), _FakePin()
        manager.pin_windows.extend([broken, healthy])
        manager.move_all_to_screen_center()
        assert len(healthy.moved_to) == 1


class TestSaveAllToDirectory:

    def test_files_are_numbered_from_one_with_the_given_prefix(self, manager, tmp_path):
        pins = [_FakePin(), _FakePin(), _FakePin()]
        manager.pin_windows.extend(pins)
        saved, failed = manager.save_all_to_directory(str(tmp_path), prefix="shot")
        assert (saved, failed) == (3, 0)
        names = [p.saved_paths[0].rsplit("\\", 1)[-1] for p in pins]
        assert names == ["shot_001.png", "shot_002.png", "shot_003.png"]

    def test_default_prefix_is_pins(self, manager, tmp_path):
        pin = _FakePin()
        manager.pin_windows.append(pin)
        manager.save_all_to_directory(str(tmp_path))
        assert pin.saved_paths[0].endswith("pins_001.png")

    def test_missing_directory_is_created(self, manager, tmp_path):
        target = tmp_path / "nested" / "deeper"
        manager.pin_windows.append(_FakePin())
        manager.save_all_to_directory(str(target))
        assert target.is_dir()

    def test_a_false_return_from_save_counts_as_a_failure(self, manager, tmp_path):
        manager.pin_windows.extend([_FakePin(save_ok=True), _FakePin(save_ok=False)])
        assert manager.save_all_to_directory(str(tmp_path)) == (1, 1)

    def test_an_exception_during_save_counts_as_a_failure(self, manager, tmp_path):
        manager.pin_windows.extend(
            [_FakePin(), _FakePin(save_raises=OSError("磁盘已满"))])
        assert manager.save_all_to_directory(str(tmp_path)) == (1, 1)

    def test_editing_is_paused_around_the_save_when_supported(self, manager, tmp_path):
        """
        正在编辑的贴图必须先把编辑状态挂起再取图，否则存下来的是带
        编辑控件的画面。没有该方法的窗口走直接保存分支。
        """
        with_pause = _FakePin(with_edit_paused=True)
        without_pause = _FakePin(with_edit_paused=False)
        manager.pin_windows.extend([with_pause, without_pause])
        saved, failed = manager.save_all_to_directory(str(tmp_path))
        assert (saved, failed) == (2, 0)
        assert with_pause.edit_paused_calls == 1
        assert len(without_pause.saved_paths) == 1

    def test_no_windows_saves_nothing(self, manager, tmp_path):
        assert manager.save_all_to_directory(str(tmp_path)) == (0, 0)


# ============================================================================
# 置顶压制
# ============================================================================

class TestSuppressTopmost:

    def test_only_windows_that_are_actually_topmost_get_demoted(self, manager, fake_user32):
        topmost, plain = _FakePin(flags=TOPMOST, hwnd=11), _FakePin(flags=PLAIN, hwnd=22)
        manager.pin_windows.extend([topmost, plain])
        manager.suppress_topmost()
        assert [call[0] for call in fake_user32.calls] == [11]
        assert fake_user32.calls[0][1] == HWND_NOTOPMOST
        assert fake_user32.calls[0][-1] == SWP_FLAGS

    def test_repeating_the_call_is_idempotent(self, manager, fake_user32):
        """
        截图流程可能重复请求压制。它是布尔开关而不是计数器，
        第二次必须直接返回——否则 _suppressed_pins 会被清空重填，
        恢复时就找不到该恢复谁了。
        """
        manager.pin_windows.append(_FakePin(hwnd=11))
        manager.suppress_topmost()
        manager.suppress_topmost()
        assert len(fake_user32.calls) == 1

    def test_a_window_pinned_after_suppression_is_left_alone(self, manager, fake_user32):
        manager.pin_windows.append(_FakePin(hwnd=11))
        manager.suppress_topmost()
        manager.pin_windows.append(_FakePin(hwnd=22))
        manager.restore_topmost()
        # 只恢复当初被压制的那一个
        assert [call[0] for call in fake_user32.calls[1:]] == [11]

    def test_restore_puts_the_demoted_windows_back_on_top(self, manager, fake_user32):
        manager.pin_windows.extend([_FakePin(hwnd=11), _FakePin(hwnd=22)])
        manager.suppress_topmost()
        fake_user32.calls.clear()
        manager.restore_topmost()
        assert [(call[0], call[1]) for call in fake_user32.calls] == [
            (11, HWND_TOPMOST), (22, HWND_TOPMOST)]

    def test_restore_without_a_prior_suppression_does_nothing(self, manager, fake_user32):
        manager.pin_windows.append(_FakePin(hwnd=11))
        manager.restore_topmost()
        assert fake_user32.calls == []

    def test_restore_skips_windows_closed_in_the_meantime(self, manager, fake_user32):
        """压制期间用户可能关掉某个贴图，对已销毁的窗口调 SetWindowPos 会出问题"""
        staying, closing = _FakePin(hwnd=11), _FakePin(hwnd=22)
        manager.pin_windows.extend([staying, closing])
        manager.suppress_topmost()
        manager.pin_windows.remove(closing)
        fake_user32.calls.clear()
        manager.restore_topmost()
        assert [call[0] for call in fake_user32.calls] == [11]

    def test_the_suppression_list_is_emptied_after_restoring(self, manager, fake_user32):
        manager.pin_windows.append(_FakePin(hwnd=11))
        manager.suppress_topmost()
        manager.restore_topmost()
        assert manager._suppressed_pins == []
        assert manager._topmost_suppressed is False

    def test_a_full_cycle_can_be_repeated(self, manager, fake_user32):
        manager.pin_windows.append(_FakePin(hwnd=11))
        for _ in range(3):
            manager.suppress_topmost()
            manager.restore_topmost()
        assert len(fake_user32.calls) == 6

    def test_suppressing_with_no_topmost_windows_records_nothing(self, manager, fake_user32):
        manager.pin_windows.append(_FakePin(flags=PLAIN))
        manager.suppress_topmost()
        assert fake_user32.calls == []
        assert manager._suppressed_pins == []
        # 开关仍然翻转，restore 才知道自己该收尾
        assert manager._topmost_suppressed is True


class TestCreatePin:

    def test_created_window_is_tracked_and_announced(self, manager, monkeypatch):
        """
        create_pin 内部是 `from pin.pin_window import PinWindow`（函数内 import），
        所以桩必须打在源模块上，打在 pin.pin_manager 命名空间是无效的。
        """
        created = []

        class _StubPinWindow:
            def __init__(self, **kwargs):
                created.append(kwargs)
                self.closed = SimpleNamespace(connect=lambda cb: None)

        monkeypatch.setattr("pin.pin_window.PinWindow", _StubPinWindow)

        announced = []
        manager.pin_created.connect(announced.append)

        pin = manager.create_pin(image=None, position=QPoint(3, 4), config_manager=None)

        assert manager.get_all_pins() == [pin]
        assert announced == [pin]
        assert created[0]["position"] == QPoint(3, 4)

    def test_optional_arguments_are_forwarded_verbatim(self, manager, monkeypatch):
        seen = {}

        class _StubPinWindow:
            def __init__(self, **kwargs):
                seen.update(kwargs)
                self.closed = SimpleNamespace(connect=lambda cb: None)

        monkeypatch.setattr("pin.pin_window.PinWindow", _StubPinWindow)
        items = [object()]
        manager.create_pin(
            image=None, position=QPoint(0, 0), config_manager=None,
            drawing_items=items, selection_offset=QPoint(5, 6), number_next=7)
        assert seen["drawing_items"] is items
        assert seen["selection_offset"] == QPoint(5, 6)
        assert seen["number_next"] == 7
