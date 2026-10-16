"""Freehand stroke expectations from the Python version's tools: Shift-line snapping, smoothing and
the element ranges each step rewrites."""
from __future__ import annotations
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import json
import math
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'main'))
from PySide6.QtCore import QPointF  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

app = QApplication([])
from tools.base import FreehandStroke, snap_line_end  # noqa: E402
from settings.tool_settings import SHIFT_LINE_ANGLES, ToolSettingsManager  # noqa: E402


def elements(path):
    return [[path.elementAt(i).x, path.elementAt(i).y, path.elementAt(i).type.value] for i in range(path.elementCount())]


def slow_then_fast():
    """Dense integer samples along a wave, then a few far-apart ones."""
    points = []
    for i in range(70):
        t = i / 69 * math.pi * 1.5
        points.append((round(40 + i * 1.7), round(100 + 20 * math.sin(t))))
    x, y = points[-1]
    for i in range(1, 9):
        points.append((x + 23 * i, y + 28 * math.sin(i * 1.3)))
    return points


def with_shift(points, start, stop, drift):
    """Shift held for steps [start, stop), the cursor wandering off the line by drift per step."""
    return [(x, y + (k - start) * drift if start <= k < stop else y, start <= k < stop) for k, (x, y) in enumerate(points)]


def replay(points, angle_step, smooth):
    stroke = FreehandStroke(QPointF(*points[0][:2]), angle_step, smooth=smooth)
    steps = []
    for x, y, straight in points[1:]:
        changed = stroke.extend(QPointF(x, y), straight=straight)
        dirty = stroke.take_dirty_from()
        last = stroke.path.elementAt(stroke.path.elementCount() - 1)
        steps.append({'pos': [x, y], 'straight': straight, 'changed': changed, 'dirty_from': dirty,
                      'count': stroke.path.elementCount(), 'last': [last.x, last.y]})
    finished = stroke.finish()
    return {'start': list(points[0][:2]), 'angle_step': angle_step, 'smooth': smooth, 'steps': steps,
            'finished': finished, 'finish_dirty_from': stroke.take_dirty_from(), 'elements': elements(stroke.path)}


base = slow_then_fast()
strokes = []
for smooth in (True, False):
    strokes.append(replay([(x, y, False) for x, y in base], 45, smooth))
    for angle in SHIFT_LINE_ANGLES:
        strokes.append(replay(with_shift(base, 30, 52, 2.5), angle, smooth))
    # Shift held from the press, released, then held again.
    strokes.append(replay([(x, y, k < 20 or 45 <= k < 60) for k, (x, y) in enumerate(base)], 90, smooth))
    # Released where it was pressed, after jitter under the point spacing, and with Shift held.
    strokes.append(replay([(100, 100, False)], 45, smooth))
    strokes.append(replay([(100, 100, False), (100.4, 100.3, False)], 45, smooth))
    strokes.append(replay([(100, 100, True), (100.4, 100.3, True)], 45, smooth))

snaps = []
for step in SHIFT_LINE_ANGLES:
    for dx, dy in [(100, 0), (100, 30), (30, -100), (-100, 10), (100, 90), (100, 100), (-70, -70), (0, 0),
                   (3, 1), (100, 20), (-40, 95), (1, 1000), (100, 26.79491924311227)]:
        anchor = QPointF(10.5, 20.25)
        end = snap_line_end(anchor, QPointF(anchor.x() + dx, anchor.y() + dy), step)
        snaps.append({'anchor': [anchor.x(), anchor.y()], 'pos': [anchor.x() + dx, anchor.y() + dy],
                      'step': step, 'end': [end.x(), end.y()]})

out = {'shift_line_angles': list(SHIFT_LINE_ANGLES),
       'default_shift_line_angle': ToolSettingsManager.APP_DEFAULT_SETTINGS['shift_line_angle'],
       'snaps': snaps, 'strokes': strokes}
target = ROOT / 'tests/fixtures/freehand.json'
target.write_text(json.dumps(out, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
print(f'{len(strokes)} strokes, {len(snaps)} snaps -> {target.relative_to(ROOT)}')
