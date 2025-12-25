"""
测试钉图窗口的矢量渲染功能
"""

import sys
import os

# 🔥 添加父目录到 Python 路径，使得可以直接运行此文件
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PyQt6.QtWidgets import QApplication
from PyQt6.QtCore import Qt, QPoint, QPointF, QRectF, QTimer
from PyQt6.QtGui import QImage, QPainter, QColor, QFont

from pin.pin_window import PinWindow
from settings import get_tool_settings_manager


def create_test_image():
    """创建测试底图"""
    image = QImage(600, 400, QImage.Format.Format_ARGB32)
    image.fill(QColor(240, 240, 240))  # 浅灰色背景
    
    # 绘制标题
    painter = QPainter(image)
    painter.setPen(Qt.GlobalColor.darkGray)
    font = painter.font()
    font.setPixelSize(20)
    painter.setFont(font)
    painter.drawText(image.rect(), Qt.AlignmentFlag.AlignCenter, "测试钉图窗口 + 矢量渲染")
    painter.end()
    
    return image


def create_test_vector_commands():
    """创建测试矢量命令"""
    commands = []
    
    # 1. 画笔（红色波浪线）
    commands.append({
        'type': 'pen',
        'points': [
            QPointF(50, 100),
            QPointF(100, 80),
            QPointF(150, 120),
            QPointF(200, 90),
            QPointF(250, 110),
        ],
        'color': QColor(255, 0, 0),
        'width': 5
    })
    
    # 2. 矩形（蓝色边框）
    commands.append({
        'type': 'rect',
        'rect': QRectF(300, 50, 150, 100),
        'color': QColor(0, 0, 255),
        'width': 3,
        'filled': False
    })
    
    # 3. 椭圆（绿色填充）
    commands.append({
        'type': 'ellipse',
        'rect': QRectF(50, 200, 120, 80),
        'color': QColor(0, 200, 0),
        'width': 3,
        'filled': True
    })
    
    # 4. 箭头（橙色）
    commands.append({
        'type': 'arrow',
        'start': QPointF(200, 250),
        'end': QPointF(350, 250),
        'color': QColor(255, 165, 0),
        'width': 4
    })
    
    # 5. 文字（紫色）
    commands.append({
        'type': 'text',
        'text': '测试文字 📝',
        'pos': QPointF(400, 250),
        'font': QFont('Arial', 16, QFont.Weight.Bold),
        'color': QColor(128, 0, 128)
    })
    
    # 6. 荧光笔（黄色半透明）
    commands.append({
        'type': 'highlighter',
        'points': [
            QPointF(50, 320),
            QPointF(200, 310),
            QPointF(350, 330),
            QPointF(500, 320),
        ],
        'color': QColor(255, 255, 0, 100),
        'width': 20
    })
    
    # 7. 序号（红色圆圈）
    commands.append({
        'type': 'number',
        'pos': QPointF(500, 100),
        'number': 1,
        'color': QColor(255, 50, 50),
        'size': 40
    })
    
    return commands


if __name__ == "__main__":
    app = QApplication(sys.argv)
    
    # 创建测试数据
    test_image = create_test_image()
    test_commands = create_test_vector_commands()
    
    print("=" * 60)
    print("🧪 测试钉图窗口 + 矢量渲染")
    print("=" * 60)
    print(f"底图尺寸: {test_image.width()}x{test_image.height()}")
    print(f"矢量命令: {len(test_commands)} 条")
    print()
    print("功能测试:")
    print("  1. 拖动窗口移动位置")
    print("  2. 滚轮缩放大小（矢量保持清晰）")
    print("  3. ESC 关闭窗口")
    print("  4. 鼠标悬停显示控制按钮")
    print("  5. 观察矢量图形是否正确渲染")
    print("=" * 60)
    
    # 创建钉图窗口（带矢量命令）
    config_manager = get_tool_settings_manager()
    pin_window = PinWindow(
        test_image,
        QPoint(100, 100),
        config_manager,
        vector_commands=test_commands
    )

    auto_close_ms = os.environ.get("PIN_TEST_AUTO_CLOSE_MS")
    if auto_close_ms:
        try:
            delay = int(auto_close_ms)
        except ValueError:
            delay = 0
        if delay > 0:
            QTimer.singleShot(delay, pin_window.close_window)
            print(f"⏱️ [测试] {delay}ms 后自动关闭钉图窗口（PIN_TEST_AUTO_CLOSE_MS）")
    
    sys.exit(app.exec())
