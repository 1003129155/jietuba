"""按布局顺序取控件时不在布局上留下条目包装：Qt 删掉条目后，不会有过期包装被同一地址的新对象认领。"""

import shiboken6
from PySide6.QtCore import QCoreApplication, QEvent
from PySide6.QtWidgets import QLabel, QPushButton, QVBoxLayout, QWidget, QWidgetItem

from core.qt_utils import layout_widgets
from ui.fluent_lite import FluentIcon, SettingCard
from ui.fluent_lite.cards import ExpandLayout
from ui.reorderable_rows import DraggableRow, ReorderableRowList


def _item_wrappers():
    return {id(obj): obj for obj in shiboken6.getAllValidWrappers() if type(obj) is QWidgetItem}


def _labels(layout_type):
    host = QWidget()
    layout = layout_type(host)
    labels = [QLabel(str(index), host) for index in range(3)]
    for label in labels:
        layout.addWidget(label)
    return host, layout, labels


def _flush_deletes():
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)


def test_widgets_come_back_in_layout_order(qapp):
    host, layout, labels = _labels(QVBoxLayout)
    layout.addStretch(1)
    layout.insertWidget(0, labels[2])
    assert layout_widgets(layout) == [labels[2], labels[0], labels[1]]


def test_no_item_wrapper_survives_the_call(qapp):
    host, layout, _labels_ = _labels(QVBoxLayout)
    before = _item_wrappers()
    layout_widgets(layout)
    assert _item_wrappers().keys() == before.keys()

    # 反向对照：直接用 itemAt 取的条目包装会一直挂在布局上，上面的判据抓得到
    layout.itemAt(0)
    leftover = [obj for key, obj in _item_wrappers().items() if key not in before]
    assert len(leftover) == 1
    shiboken6.invalidate(leftover[0])


def test_layout_passes_and_row_lists_leave_no_item_wrappers(qapp):
    host, layout, labels = _labels(ExpandLayout)
    card = SettingCard(FluentIcon.INFO, "Title")
    card.addControl(QPushButton("A"))
    rows = ReorderableRowList()
    for _ in range(3):
        rows.add_row(DraggableRow())
    # addLayout 时 PySide 自己会给子布局的条目建包装，这里只看取控件的这几个调用
    before = _item_wrappers()

    layout.heightForWidth(300)
    card.controlColumnHint()
    assert len(rows.rows()) == 3
    rows.clear()
    labels[0].deleteLater()
    _flush_deletes()

    assert _item_wrappers().keys() <= before.keys()
