"""Developer-only oracle for barcode/reader: an image with several codes and the reference's decoding.

Run with the reference interpreter:
    ..\\venv311\\Scripts\\python.exe scripts\\generate_barcode_fixtures.py
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # the reference tree tracks its __pycache__
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'main'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import zxingcpp  # noqa: E402
from PySide6.QtCore import QPoint  # noqa: E402
from PySide6.QtGui import QColor, QImage, QPainter, QTransform  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from barcode.reader import read_codes  # noqa: E402

app = QApplication([])


def code_image(content, format_, scale):
    barcode = zxingcpp.create_barcode(content, format_)
    rendered = zxingcpp.write_barcode_to_image(barcode, scale=scale)
    height, width = rendered.shape[:2]
    return QImage(bytes(memoryview(rendered)), width, height, width, QImage.Format.Format_Grayscale8).copy()


canvas = QImage(900, 560, QImage.Format.Format_ARGB32_Premultiplied)
canvas.fill(QColor('white'))
painter = QPainter(canvas)
# Placed out of reading order, with uneven top edges in the first row.
placements = [
    ('https://example.com/a', zxingcpp.BarcodeFormat.QRCode, 4, QPoint(620, 40), 0),
    ('Second code', zxingcpp.BarcodeFormat.QRCode, 3, QPoint(40, 60), 0),
    ('CODE128-0042', zxingcpp.BarcodeFormat.Code128, 2, QPoint(330, 70), 0),
    ('中文内容', zxingcpp.BarcodeFormat.QRCode, 3, QPoint(80, 330), 12),
    ('DM text', zxingcpp.BarcodeFormat.DataMatrix, 6, QPoint(560, 340), 0),
]
for content, format_, scale, origin, angle in placements:
    image = code_image(content, format_, scale)
    if angle:
        image = image.transformed(QTransform().rotate(angle))
    painter.drawImage(origin, image)
painter.end()

target_dir = ROOT / 'tests' / 'fixtures'
canvas.save(str(target_dir / 'barcodes.png'))
reloaded = QImage(str(target_dir / 'barcodes.png'))
codes = [{'text': code.text, 'format_name': code.format_name,
          'outline': [[point.x(), point.y()] for point in code.outline]} for code in read_codes(reloaded)]
(target_dir / 'barcodes.json').write_text(json.dumps(codes, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
print(f"{len(codes)} codes -> tests/fixtures/barcodes.json")
