"""文字识别结果窗口的摆放：只算位置，不碰窗口。

screens 里的对象只需要有 geometry() 和 availableGeometry()，QScreen 即可。
"""

from PySide6.QtCore import QPoint, QRect

# 结果窗口与识别区域之间的空隙，像素
GAP = 12
# 结果窗口摆在光标旁边时与光标的距离，像素
CURSOR_OFFSET = 16


def pick_screen(position, anchor, cursor, last_center, screens, primary):
    """结果窗口摆在哪块屏幕上。

    anchor 是识别区域（全局屏幕坐标的 QRect），last_center 是上次关闭时窗口的中心。
    要看的点不在任何屏幕上就依次退到下一个，最后是主屏。
    """
    if position == "primary_screen":
        return primary
    region = anchor.center() if anchor is not None else None
    if position == "last_position":
        points = (last_center, region, cursor)
    elif position == "near_cursor":
        points = (cursor, region)
    else:
        points = (region, cursor)
    for point in points:
        if point is None:
            continue
        for screen in screens:
            if screen.geometry().contains(point):
                return screen
    return primary


def window_position(position, size, screen, anchor=None, cursor=None, last_center=None):
    """窗口左上角的位置，screen 是 pick_screen 选出的屏幕。

    beside_region 贴在识别区域旁边，按右、左、下、上的顺序找放得下的一侧；near_cursor 在光标
    右下，放不下就翻到另一侧；last_position 让窗口中心回到上次的位置。条件不满足（没有识别
    区域、光标或记录不在 screen 上）或识别区域四周都放不下时，摆在 screen 可用区域的正中。
    """
    available = screen.availableGeometry()
    if position == "beside_region" and anchor is not None:
        beside = _beside(anchor, size, available)
        if beside is not None:
            return beside
    elif position == "near_cursor" and cursor is not None and screen.geometry().contains(cursor):
        return _near(cursor, size, available)
    elif position == "last_position" and last_center is not None and screen.geometry().contains(last_center):
        top_left = last_center - QRect(QPoint(0, 0), size).center()
        return QPoint(
            _clamp(top_left.x(), available.left(), available.right() + 1 - size.width()),
            _clamp(top_left.y(), available.top(), available.bottom() + 1 - size.height()),
        )
    return available.center() - QRect(QPoint(0, 0), size).center()


def _beside(anchor, size, available):
    width, height = size.width(), size.height()
    # 左右并排时顶边对齐识别区域，方便逐行对照；上下并排时左边对齐
    top = _clamp(anchor.top(), available.top(), available.bottom() + 1 - height)
    left = _clamp(anchor.left(), available.left(), available.right() + 1 - width)

    x = anchor.right() + 1 + GAP
    if x + width <= available.right() + 1:
        return QPoint(x, top)
    x = anchor.left() - GAP - width
    if x >= available.left():
        return QPoint(x, top)
    y = anchor.bottom() + 1 + GAP
    if y + height <= available.bottom() + 1:
        return QPoint(left, y)
    y = anchor.top() - GAP - height
    if y >= available.top():
        return QPoint(left, y)
    return None


def _near(cursor, size, available):
    width, height = size.width(), size.height()
    x = cursor.x() + CURSOR_OFFSET
    if x + width > available.right() + 1:
        x = cursor.x() - CURSOR_OFFSET - width
    y = cursor.y() + CURSOR_OFFSET
    if y + height > available.bottom() + 1:
        y = cursor.y() - CURSOR_OFFSET - height
    return QPoint(
        _clamp(x, available.left(), available.right() + 1 - width),
        _clamp(y, available.top(), available.bottom() + 1 - height),
    )


def _clamp(value, low, high):
    return max(low, min(value, high))
