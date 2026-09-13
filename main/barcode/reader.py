"""二维码 / 条形码解码

只回答"图里有哪些码、各在哪"，与界面无关；展示见 result_window.py。
"""

from dataclasses import dataclass

import zxingcpp
from PySide6.QtCore import QPointF
from PySide6.QtGui import QImage, QPolygonF


@dataclass(frozen=True)
class DecodedCode:
    text: str
    format_name: str    # 给人看的格式名，如 "QR Code"、"Code 128"
    outline: QPolygonF  # 码在原图上的四个角（像素坐标）；码拍斜了，这里就是斜的四边形


def read_codes(image: QImage) -> list[DecodedCode]:
    """识别 image 里所有的码，按阅读顺序排好；一个都没有就返回空列表。

    zxing-cpp 能直接收 QImage（不必转 numpy / PIL，打包时 numpy 本来就被排除了），但只直接读
    RGB32、ARGB32、RGB888、Grayscale8 等几种格式；其余格式它会调 convertToFormat(int) 自己转，
    PySide6 不接受整数形式的格式参数，直接抛 TypeError——截图导出的底图 ARGB32_Premultiplied
    正在其中。所以统一先转成 Grayscale8 再交给它：解码只看亮度，内存也只有 32 位图的四分之一。
    """
    gray = image.convertToFormat(QImage.Format.Format_Grayscale8)
    codes = [
        DecodedCode(barcode.text, str(barcode.format), _outline(barcode.position))
        for barcode in zxingcpp.read_barcodes(gray)
    ]
    return _reading_order(codes)


def _outline(position) -> QPolygonF:
    corners = (position.top_left, position.top_right, position.bottom_right, position.bottom_left)
    return QPolygonF([QPointF(corner.x, corner.y) for corner in corners])


def _reading_order(codes: list[DecodedCode]) -> list[DecodedCode]:
    """从上到下、同一行从左到右，让序号顺着看过去就是 1、2、3。

    zxing 返回的先后与位置无关（实测横排的五个码按 4、2、1、3、5 的次序返回），直接编号
    会跳着来；而一排码的顶边很少严格对齐，单按顶边排同样会打乱。所以竖直方向和当前行
    有重叠的码归入这一行，行内再按左边排。
    """
    rows = []   # [[这一行目前最低的底边, [码, ...]], ...]
    for code in sorted(codes, key=lambda item: item.outline.boundingRect().top()):
        rect = code.outline.boundingRect()
        if rows and rect.top() < rows[-1][0]:
            rows[-1][0] = max(rows[-1][0], rect.bottom())
            rows[-1][1].append(code)
        else:
            rows.append([rect.bottom(), [code]])
    return [
        code
        for _bottom, row in rows
        for code in sorted(row, key=lambda item: item.outline.boundingRect().left())
    ]
