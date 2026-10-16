"""Developer-only oracle for stitch/incremental: frames cut from a synthetic page and
the reference IncrementalStitcher's results for them.

Run with the reference interpreter:
    ..\\venv311\\Scripts\\python.exe scripts\\generate_stitch_fixtures.py
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import sys
import threading
from pathlib import Path

sys.dont_write_bytecode = True  # the reference tree tracks its __pycache__
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'main'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from PySide6.QtCore import QRect, Qt  # noqa: E402
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QTransform  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from stitch.incremental import Frame, IncrementalStitcher  # noqa: E402

app = QApplication([])
random.seed(20261013)

WIDTH, PAGE_HEIGHT, FRAME_HEIGHT = 360, 2400, 300


def page_image():
    """A page of distinct text rows, so every offset matches one place."""
    image = QImage(WIDTH, PAGE_HEIGHT, QImage.Format.Format_RGB32)
    image.fill(QColor('white'))
    painter = QPainter(image)
    painter.setFont(QFont('Arial', 11))
    for row in range(PAGE_HEIGHT // 24):
        painter.setPen(QColor(random.randint(0, 120), random.randint(0, 120), random.randint(0, 120)))
        painter.drawText(QRect(8, row * 24, WIDTH - 40, 24), 0, f'Row {row:03d} ' + ''.join(random.choice('abcdefghij ') for _ in range(24)))
        painter.fillRect(WIDTH - 30, row * 24 + 4, random.randint(4, 20), 12, QColor(random.randint(0, 255), 80, 160))
    painter.end()
    return image


def frame_image(top, direction):
    """Vertical frames are WIDTH x FRAME_HEIGHT from the page; horizontal ones are
    FRAME_HEIGHT x WIDTH from the page turned counter-clockwise."""
    if direction == 'horizontal':
        return WIDE.copy(top, 0, FRAME_HEIGHT, WIDTH)
    return PAGE.copy(0, top, WIDTH, FRAME_HEIGHT)


PAGE = page_image()
WIDE = PAGE.transformed(QTransform().rotate(-90))


def run(tops, direction, headings):
    results = []
    done = threading.Event()
    stitcher = IncrementalStitcher(96, 20, 0)

    def collect(result):
        results.append({
            'ok': result.ok, 'first': result.first, 'index': result.index, 'width': result.width,
            'height': result.height, 'top': result.top, 'shift': result.shift,
            'box': list(result.box) if result.box else None, 'size': list(result.size) if result.size else None,
            'preview': [result.preview.width(), result.preview.height()] if result.preview is not None else None,
        })
        if len(results) == len(tops):
            done.set()

    # Emitted on the worker thread while this thread waits, so delivered directly.
    stitcher.frame_done.connect(collect, Qt.ConnectionType.DirectConnection)
    for index, top in enumerate(tops):
        image = frame_image(top, direction)
        data = bytes(image.constBits())[:image.width() * image.height() * 4]
        stitcher.submit(Frame(data, image.width(), image.height(), index, direction, headings[index]))
    done.wait(30)
    image = stitcher.export()
    rgb = image.convert('RGB')
    stitcher.close()
    return results, {'size': list(rgb.size), 'sha256': hashlib.sha256(rgb.tobytes()).hexdigest()}


cases = []
for name, direction, tops, headings in [
    ('down', 'vertical', [0, 120, 260, 400, 520], [None, 'down', 'down', None, None]),
    ('mid_up_then_down', 'vertical', [900, 760, 640, 900, 1100], [None, 'up', None, 'down', None]),
    ('no_overlap', 'vertical', [0, 150, 1500, 300], [None, None, None, None]),
    ('still', 'vertical', [600, 600, 600], [None, None, None]),
    ('horizontal', 'horizontal', [0, 130, 270, 400], [None, None, 'down', None]),
]:
    results, export = run(tops, direction, headings)
    cases.append({'name': name, 'direction': direction, 'tops': tops, 'headings': headings, 'results': results, 'export': export})

target_dir = ROOT / 'tests' / 'fixtures'
PAGE.save(str(target_dir / 'stitch_page.png'))
(target_dir / 'stitch.json').write_text(json.dumps({'width': WIDTH, 'frame_height': FRAME_HEIGHT, 'cases': cases}, indent=1) + '\n', encoding='utf-8')
print(f"{len(cases)} cases -> tests/fixtures/stitch.json")
