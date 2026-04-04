# -*- coding: utf-8 -*-
"""
GIF 帧数据结构单元测试

测试 FrameData / CursorSnapshot / RecordState 数据类的纯逻辑。
"""
import pytest
from gif.frame_recorder import FrameData, CursorSnapshot, RecordState


class TestRecordState:
    """录制状态枚举测试"""

    def test_states_exist(self):
        """所有状态应存在"""
        assert RecordState.IDLE is not None
        assert RecordState.RECORDING is not None
        assert RecordState.PAUSED is not None
        assert RecordState.STOPPED is not None

    def test_states_unique(self):
        """状态值应唯一"""
        states = [RecordState.IDLE, RecordState.RECORDING, RecordState.PAUSED, RecordState.STOPPED]
        assert len(set(states)) == 4


class TestCursorSnapshot:
    """鼠标状态快照测试"""

    def test_create_snapshot(self):
        """创建快照"""
        snap = CursorSnapshot(x=100, y=200, press=0)
        assert snap.x == 100
        assert snap.y == 200
        assert snap.press == 0
        assert snap.visible is True  # 默认值
        assert snap.scroll == 0      # 默认值

    def test_left_click(self):
        """左键按下"""
        snap = CursorSnapshot(x=0, y=0, press=1)
        assert snap.press == 1

    def test_right_click(self):
        """右键按下"""
        snap = CursorSnapshot(x=0, y=0, press=2)
        assert snap.press == 2

    def test_not_visible(self):
        """光标不可见"""
        snap = CursorSnapshot(x=-10, y=-10, press=0, visible=False)
        assert not snap.visible

    def test_scroll_up(self):
        """向上滚动"""
        snap = CursorSnapshot(x=0, y=0, press=0, scroll=1)
        assert snap.scroll == 1

    def test_scroll_down(self):
        """向下滚动"""
        snap = CursorSnapshot(x=0, y=0, press=0, scroll=-1)
        assert snap.scroll == -1


class TestFrameData:
    """帧元数据测试"""

    def test_create_frame(self):
        """创建帧数据"""
        frame = FrameData(elapsed_ms=100)
        assert frame.elapsed_ms == 100
        assert frame.width == 0
        assert frame.height == 0
        assert frame.annotations == []
        assert frame.cursor is None

    def test_frame_with_cursor(self):
        """带鼠标信息的帧"""
        cursor = CursorSnapshot(x=50, y=60, press=0)
        frame = FrameData(elapsed_ms=200, width=1920, height=1080, cursor=cursor)
        assert frame.cursor is not None
        assert frame.cursor.x == 50

    def test_frame_with_annotations(self):
        """带标注的帧"""
        frame = FrameData(elapsed_ms=300, annotations=["rect", "arrow"])
        assert len(frame.annotations) == 2

    def test_annotations_default_factory(self):
        """多个帧的默认标注列表应独立"""
        f1 = FrameData(elapsed_ms=0)
        f2 = FrameData(elapsed_ms=100)
        f1.annotations.append("test")
        assert len(f2.annotations) == 0  # 不应被 f1 影响

    def test_frame_sequence(self):
        """帧序列时间递增"""
        frames = [
            FrameData(elapsed_ms=0),
            FrameData(elapsed_ms=100),
            FrameData(elapsed_ms=200),
        ]
        for i in range(1, len(frames)):
            assert frames[i].elapsed_ms > frames[i - 1].elapsed_ms

    def test_total_duration(self):
        """帧序列总时长"""
        frames = [FrameData(elapsed_ms=ms) for ms in [0, 100, 200, 300, 500]]
        total = frames[-1].elapsed_ms
        assert total == 500
 