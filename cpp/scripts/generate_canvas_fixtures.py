"""Independent drawing/undo expectations from the frozen Python implementation."""
from __future__ import annotations
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import sys, json, hashlib, random, argparse
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--output', default='tests/fixtures/canvas.json')
parser.add_argument('--platform', choices=['offscreen', 'windows'])
arguments = parser.parse_args()
if arguments.platform: os.environ['QT_QPA_PLATFORM'] = arguments.platform
sys.path.insert(0, str(ROOT.parent / 'main'))
from PySide6.QtCore import Qt, QPointF, QRectF, qVersion, Signal
from PySide6.QtGui import QColor, QPen, QPainterPath, QImage, QPainter, QTransform, QFontInfo
from PySide6.QtWidgets import QApplication, QGraphicsScene, QGraphicsView, QGraphicsItem, QStyleOptionGraphicsItem
from canvas.items import StrokeItem, RectItem, EllipseItem, NumberItem, ArrowItem
from canvas import undo
from tools.number import NumberTool
app = QApplication([])

def rect(r): return [r.x(), r.y(), r.width(), r.height()]
def point(p): return [p.x(), p.y()]
def create(config):
    pen = QPen(QColor(config.get('color', '#d02060')), config.get('width', 4))
    pen.setStyle(Qt.PenStyle(config.get('pen_style', 1)))
    kind = config['kind']
    if kind == 'stroke':
        path = QPainterPath(QPointF(20, 30))
        if not config.get('empty', False):
            path.cubicTo(40, 80, 90, 0, 120, 70)
        item = StrokeItem(path, pen, config.get('highlighter', False))
    elif kind == 'rect':
        item = RectItem(QRectF(20, 20, 100, 60), pen, config.get('radius', 0))
        item.is_highlighter = config.get('highlighter', False)
        item.is_highlighter_rect = config.get('filled', False)
        if item.is_highlighter_rect: item.setBrush(QColor('#40a060'))
    elif kind == 'ellipse': item = EllipseItem(QRectF(20, 20, 100, 60), pen)
    elif kind == 'arrow': item = ArrowItem(QPointF(*config.get('start', [20, 40])), QPointF(*config.get('end', [130, 70])), pen, config.get('style', 'single'))
    else: item = NumberItem(config.get('number', 12), QPointF(75, 55), config.get('radius', 20), pen.color(), config.get('style'))
    return item

class Controller:
    def __init__(self): self.selected_item = None; self.allow = False
    def can_show_hover_cursor(self, item): return self.allow

def drawing_snapshot(item):
    shape = item.shape()
    elements = [[shape.elementAt(i).x, shape.elementAt(i).y, shape.elementAt(i).type.value] for i in range(shape.elementCount())]
    style = item.selection_frame_style()
    image = QImage(160, 120, QImage.Format_ARGB32_Premultiplied); image.fill(QColor('#bfd2e8'))
    painter = QPainter(image); painter.translate(item.pos()); painter.setOpacity(item.opacity())
    item.paint(painter, QStyleOptionGraphicsItem(), None); painter.end()
    out = {'bounds': rect(item.boundingRect()), 'shape': elements,
           'width': item.get_stroke_width(), 'opacity': item.get_visual_opacity(),
           'item_opacity': item.opacity(), 'z': item.zValue(),
           'movable': bool(item.flags() & QGraphicsItem.ItemIsMovable), 'selectable': bool(item.flags() & QGraphicsItem.ItemIsSelectable),
           'frame': None if style is None else style.value, 'cursor': item.cursor().shape().value,
           'sha256': hashlib.sha256(bytes(image.constBits())).hexdigest()}
    if isinstance(item, NumberItem):
        glyphs = QPainterPath(); glyphs.addText(QPointF(0, 0), item._number_font(), str(item.number))
        out.update(style=item.style, hollow=item.is_hollow, ring=item.has_ring,
                   visual=rect(item.visualRect()), scene_visual=rect(item.sceneVisualRect()),
                   radius=item.radius, text_color=item._solid_text_color().name(), font_size=item._number_font().pointSize(),
                   ring_width=item._ring_width(), font_family=QFontInfo(item._number_font()).family(), glyph_elements=glyphs.elementCount())
    if isinstance(item, ArrowItem):
        path = item.path()
        out.update(arrow_style=item.arrow_style, start=point(item.start_pos), end=point(item.end_pos), control=point(item.control_pos),
                   stored_control=point(item._control_pos), curved=item.is_curved(), color=item.color.name(QColor.HexArgb),
                   path=[[path.elementAt(i).x, path.elementAt(i).y, path.elementAt(i).type.value] for i in range(path.elementCount())],
                   path_bounds=rect(path.boundingRect()))
    return out

drawing_cases = []
configs = [{'kind': k, 'width': width} for k in ['stroke', 'rect', 'ellipse'] for width in [0, 1, 4.5, 12]]
configs += [{'kind': 'stroke', 'highlighter': True}, {'kind': 'stroke', 'empty': True}]
configs += [{'kind': 'rect', 'radius': r, 'pen_style': s, 'highlighter': filled, 'filled': filled}
            for r in [0, 12, 120] for s in [1, 2, 6] for filled in [False, True]]
configs += [{'kind': 'number', 'radius': r, 'style': s, 'color': c, 'number': n}
            for r in [4, 20, 32] for s in ['solid', 'hollow_bg', 'hollow_all', 'no_circle', 'bad']
            for c, n in [('#ffffff', 7), ('#d02060', 123)]]
arrow_configs = [{'kind': 'arrow', 'width': width, 'style': style, 'start': start, 'end': end}
                 for style in list(ArrowItem.STYLES) + ['bad'] for width in [0, 1, 4.5, 20]
                 for start, end in [([20, 40], [130, 70]), ([75, 50], [78, 51]), ([90, 80], [20, 25]), ([50, 50], [50, 50])]]
configs += arrow_configs
for config in configs:
    scene = QGraphicsScene(); view = QGraphicsView(scene); controller = Controller(); view.smart_edit_controller = controller
    item = create(config); scene.addItem(item)
    actions = [{'op': 'initial'}, {'op': 'width', 'value': -3}, {'op': 'width', 'value': 8.5},
               {'op': 'scale', 'value': 1.5}, {'op': 'opacity', 'value': .37}, {'op': 'direct_opacity', 'value': .998},
               {'op': 'direct_opacity', 'value': .999}, {'op': 'opacity', 'value': 1.2},
               {'op': 'hover', 'allow': True}, {'op': 'move_hover'}, {'op': 'select'}, {'op': 'deselect'},
               {'op': 'allow', 'value': False}, {'op': 'leave'}]
    if config['kind'] == 'rect': actions += [{'op': 'rect'}, {'op': 'corner', 'value': 17}]
    if config['kind'] == 'ellipse': actions += [{'op': 'rect'}]
    if config['kind'] == 'stroke': actions += [{'op': 'path'}]
    if config['kind'] == 'arrow':
        actions += [{'op': 'control_value', 'value': [65, -20]}, {'op': 'geometry'},
                    {'op': 'control', 'value': [65, -20]}, {'op': 'positions', 'start': [25, 80], 'end': [120, 25]},
                    {'op': 'control_value', 'value': [40, 110]}, {'op': 'geometry'}, {'op': 'control_value', 'value': None}, {'op': 'geometry'},
                    {'op': 'control', 'value': [20, 120]}, {'op': 'reset'}, {'op': 'positions', 'start': [40, 40], 'end': [40.01, 40]},
                    {'op': 'positions', 'start': [10, 50], 'end': [140, 50]}, {'op': 'arrow_style', 'value': 'bad'},
                    {'op': 'arrow_style', 'value': 'double'}, {'op': 'arrow_style', 'value': 'hollow'},
                    {'op': 'positions', 'start': [20, 40], 'end': [130, 70]},
                    {'op': 'control', 'value': [47.5, 47.5]}, {'op': 'control', 'value': [102.5, 62.5]}]
    steps = []
    for action in actions:
        op = action['op']
        if op == 'width': item.set_stroke_width(action['value'])
        elif op == 'scale': item.scale_stroke_width(action['value'])
        elif op == 'opacity': item.set_visual_opacity(action['value'])
        elif op == 'direct_opacity': item.setOpacity(action['value'])
        elif op == 'hover': controller.allow = action['allow']; item.hoverEnterEvent(None)
        elif op == 'move_hover': item.hoverMoveEvent(None)
        elif op == 'select': controller.selected_item = item
        elif op == 'deselect': controller.selected_item = None
        elif op == 'allow': controller.allow = action['value']
        elif op == 'leave': item.hoverLeaveEvent(None)
        elif op == 'rect': item.setRect(QRectF(25, 15, 90, 70))
        elif op == 'corner': item.set_corner_radius(action['value'])
        elif op == 'path':
            path = QPainterPath(QPointF(30, 40)); path.lineTo(100, 90); item.setPath(path)
        elif op == 'control_value': item.control_pos = None if action['value'] is None else QPointF(*action['value'])
        elif op == 'geometry': item.update_geometry()
        elif op == 'control': item.set_control_point(QPointF(*action['value']))
        elif op == 'positions': item.set_positions(QPointF(*action['start']), QPointF(*action['end']))
        elif op == 'reset': item.reset_control_point()
        elif op == 'arrow_style': item.arrow_style = action['value']
        steps.append({'action': action, 'expected': drawing_snapshot(item)})
    drawing_cases.append({'config': config, 'steps': steps})
    scene.clear()

class UndoScene(QGraphicsScene):
    cursor_tool_update_requested = Signal(str, bool)

def undo_oracle(actions):
    scene = UndoScene(); stack = undo.CommandUndoStack(); items = []; steps = []; events = []
    view = QGraphicsView(scene)
    def record(kind, value):
        events.append({'kind': kind, 'value': value, 'index': stack.index(),
                       'next': NumberTool.get_next_number(scene), 'numbers': [item.number for item in items if item.scene() is scene]})
    class Toolbar:
        def set_number_next_value(self, value): record('toolbar', value)
    view.toolbar = Toolbar()
    class Tool: id = 'number'
    class ToolController: current_tool = Tool()
    scene.tool_controller = ToolController()
    scene.cursor_tool_update_requested.connect(lambda tool, force: record('cursor', [tool, force]))
    now = [100.0]; original_clock = undo.time.monotonic; undo.time.monotonic = lambda: now[0]
    try:
        for action in actions:
            events.clear(); op = action['op']; now[0] += action.get('dt', .1)
            if op == 'add':
                item = NumberItem(action['number'], QPointF(len(items)*10, 0), 20, QColor('#d02060'))
                if 'order' in action: item.number_order = action['order']
                items.append(item)
                stack.push_command(undo.AddNumberCommand(scene, item, action.get('before'), action.get('after')))
            elif op in ['remove', 'renumber']:
                cls = undo.RemoveNumberCommand if op == 'remove' else undo.RemoveNumberAndRenumberCommand
                stack.push_command(cls(scene, items[action['item']]))
            elif op == 'batch': stack.push_command(undo.BatchRemoveCommand(scene, [items[i] for i in action['items']], number_next_before=action.get('before')))
            elif op == 'style':
                item = items[action['item']]; stack.push_command(undo.NumberStyleCommand(item, item.style, action['value'], 'Style'))
            elif op == 'edit':
                item = items[action['item']]; before = NumberTool.get_next_number(scene)
                after = NumberTool.get_next_after_number_edit(scene, item, item.number, action['value'])
                stack.push_command(undo.NumberEditCommand(item, {'number': item.number}, {'number': action['value']}, before, after))
            elif op == 'next': NumberTool.set_next_number(scene, action['value'])
            elif op == 'adjust': NumberTool.adjust_next_number(scene, action['value'])
            elif op == 'tool': scene.tool_controller.current_tool.id = action['value']
            elif op == 'order': NumberTool.assign_number_order(scene, items[action['item']])
            else: getattr(stack, op)()
            steps.append({'action': action, 'expected': {
                'count': stack.count(), 'index': stack.index(), 'can_undo': stack.canUndo(), 'can_redo': stack.canRedo(),
                'undo_text': stack.undoText(), 'redo_text': stack.redoText(), 'next': NumberTool.get_next_number(scene),
                'offset': getattr(scene, NumberTool._SCENE_OFFSET_ATTR, 0),
                'events': list(events),
                'items': [{'attached': item.scene() is scene, 'number': item.number, 'style': item.style, 'order': item.number_order} for item in items],
                'draw_order': [items.index(item) for item in reversed(scene.items())]}})
    finally:
        undo.time.monotonic = original_clock
    stack.clear(); scene.clear()
    return steps

actions = [{'op': 'undo'}, {'op': 'redo'}, {'op': 'add', 'number': 5}, {'op': 'add', 'number': 5, 'order': 8},
           {'op': 'add', 'number': 8, 'order': 2}, {'op': 'next', 'value': 12}, {'op': 'remove', 'item': 1},
           {'op': 'undo'}, {'op': 'renumber', 'item': 2}, {'op': 'undo'}, {'op': 'redo'}, {'op': 'undo'},
           {'op': 'batch', 'items': [0, 1], 'before': 20}, {'op': 'undo'}, {'op': 'next', 'value': 30},
           {'op': 'redo'}, {'op': 'undo'}, {'op': 'style', 'item': 0, 'value': 'hollow_all'},
           {'op': 'edit', 'item': 0, 'value': 15}, {'op': 'edit', 'item': 0, 'value': 16, 'dt': .7},
           {'op': 'edit', 'item': 0, 'value': 17, 'dt': .700001}, {'op': 'edit', 'item': 1, 'value': 10},
           {'op': 'undo'}, {'op': 'undo'}, {'op': 'redo'}, {'op': 'redo'}, {'op': 'clear'}, {'op': 'undo'},
           {'op': 'tool', 'value': 'eraser'}, {'op': 'add', 'number': 0, 'before': 0, 'after': 0},
           {'op': 'undo'}, {'op': 'redo'}, {'op': 'adjust', 'value': -20}, {'op': 'adjust', 'value': 10}, {'op': 'adjust', 'value': 0},
           {'op': 'tool', 'value': 'number'}, {'op': 'add', 'number': 4, 'before': 7, 'after': 19}]
undo_cases = [undo_oracle(actions)]
rng = random.Random(20261013)
for _ in range(24):
    actions = []; count = 0
    for _ in range(60):
        op = rng.choice(['add', 'add', 'remove', 'renumber', 'batch', 'edit', 'style', 'undo', 'redo', 'next', 'order']) if count else 'add'
        action = {'op': op}
        if op == 'add': action['number'] = rng.randrange(-2, 20); count += 1
        if op in ['remove', 'renumber', 'edit', 'style', 'order']: action['item'] = rng.randrange(count)
        if op in ['edit', 'next']: action['value'] = rng.randrange(-2, 30)
        if op == 'style': action['value'] = rng.choice(['solid', 'hollow_all', 'no_circle', 'bad'])
        if op == 'batch': action['items'] = rng.sample(range(count), min(count, 3)); action['before'] = rng.choice([None, 0, 12])
        action['dt'] = rng.choice([0, .1, .7, .8]); actions.append(action)
    undo_cases.append(undo_oracle(actions))
def decode_state(data):
    out = dict(data)
    for key in ['pos', 'transformOriginPoint', 'start', 'end', 'control']:
        if isinstance(out.get(key), list): out[key] = QPointF(*out[key])
    if isinstance(out.get('rect'), list): out['rect'] = QRectF(*out['rect'])
    if isinstance(out.get('transform'), list): out['transform'] = QTransform(*out['transform'])
    if isinstance(out.get('pen_state'), dict):
        out['pen_state'] = dict(out['pen_state'])
        if 'pen_color' in out['pen_state']: out['pen_state']['pen_color'] = QColor(out['pen_state']['pen_color'])
        for key, enum in [('pen_style', Qt.PenStyle), ('pen_cap_style', Qt.PenCapStyle), ('pen_join_style', Qt.PenJoinStyle)]:
            if key in out['pen_state']: out['pen_state'][key] = enum(out['pen_state'][key])
    return out
def edit_snapshot(item):
    out = drawing_snapshot(item)
    t = item.transform()
    out.update(pos=point(item.pos()), transform=[t.m11(), t.m12(), t.m13(), t.m21(), t.m22(), t.m23(), t.m31(), t.m32(), t.m33()],
               rotation=item.rotation(), origin=point(item.transformOriginPoint()))
    if hasattr(item, 'pen'):
        p = item.pen(); out['pen'] = {'color': p.color().name(QColor.HexArgb), 'width': p.widthF(),
            'style': p.style().value, 'cap': p.capStyle().value, 'join': p.joinStyle().value, 'dash': p.dashPattern()}
    if hasattr(item, 'rect'): out['rect'] = rect(item.rect())
    if isinstance(item, NumberItem): out['number'] = item.number
    if isinstance(item, RectItem): out['corner'] = item.get_corner_radius()
    return out
edit_cases = []
states = [
    {'pos': [-10, 8], 'rotation': 33.5, 'transformOriginPoint': [12, 5], 'opacity': .45,
     'transform': [1.2, .1, 0, -.2, .9, 0, 3, -4, 1], 'rect': [18, 12, 70, 80], 'corner_radius': 9,
     'number': -5, 'pen_state': {'pen_color': '#8033aa11', 'pen_width': 7.5, 'pen_style': 6,
                               'pen_cap_style': 16, 'pen_join_style': 128, 'pen_dash_pattern': [2, 1, 4, 3]}},
    {'pos': 'invalid', 'rotation': '45', 'opacity': '0.25', 'rect': 'invalid', 'number': 2.5,
     'pen_state': {}, 'corner_radius': '17.5'},
    {'rotation': True, 'number': False, 'opacity': False},
    {'pen_state': {'pen_color': '#2a77de', 'pen_width': 0, 'pen_style': 2}},
]
for kind in ['stroke', 'rect', 'ellipse', 'number']:
    for raw in states:
        config = {'kind': kind}; scene = QGraphicsScene(); item = create(config); scene.addItem(item)
        old_raw = {'pos': [0, 0], 'rotation': 0, 'transformOriginPoint': [0, 0], 'transform': [1, 0, 0, 0, 1, 0, 0, 0, 1],
                   'opacity': 1., 'rect': [20, 20, 100, 60], 'number': 12, 'corner_radius': 0}
        old, new = decode_state(old_raw), decode_state(raw)
        command = undo.EditItemCommand(item, old, new)
        # Mutate Qt values after construction: the command must retain its copy.
        for state in [old, new]:
            if isinstance(state.get('pos'), QPointF): state['pos'].setX(999)
            if isinstance(state.get('rect'), QRectF): state['rect'].setWidth(999)
            if isinstance(state.get('transform'), QTransform): state['transform'].translate(999, 999)
        stack = undo.CommandUndoStack(); steps = []
        for op in ['push', 'undo', 'redo', 'undo']:
            if op == 'push': stack.push_command(command)
            else: getattr(stack, op)()
            steps.append({'op': op, 'expected': edit_snapshot(item)})
        edit_cases.append({'config': config, 'old': old_raw, 'new': raw, 'steps': steps})
        stack.clear(); scene.clear()
for style in ArrowItem.STYLES:
    arrow_states = [
        {'start': [10, 80], 'end': [145, 40], 'control': [65, -10], 'control_modified': True, 'arrow_style': 'hollow', 'pos': [3, 7], 'rotation': 20},
        {'start': [10, 20], 'end': [140, 90], 'control': [65, -10]},
        {'control': [65, -10], 'control_modified': False},
        {'control': None, 'control_modified': True}, {'control_modified': 'false'},
        {'control_modified': []}, {'control_modified': {}}, {'control_modified': 0},
        {'start': [40, 40], 'end': [40, 40]},
        {'arrow_style': 'unknown'}, {'arrow_style': None},
        {'pen_state': {'pen_width': 9, 'pen_color': '#334488'}},
    ]
    for raw in arrow_states:
        config = {'kind': 'arrow', 'style': style}; scene = QGraphicsScene(); item = create(config); scene.addItem(item)
        old_raw = {'start': [20, 40], 'end': [130, 70], 'control': [75, 55], 'control_modified': False, 'arrow_style': style,
                   'pos': [0, 0], 'rotation': 0}
        stack = undo.CommandUndoStack(); command = undo.EditItemCommand(item, decode_state(old_raw), decode_state(raw)); steps = []
        for op in ['push', 'undo', 'redo', 'undo']:
            if op == 'push': stack.push_command(command)
            else: getattr(stack, op)()
            steps.append({'op': op, 'expected': edit_snapshot(item)})
        edit_cases.append({'config': config, 'old': old_raw, 'new': raw, 'steps': steps})
        stack.clear(); scene.clear()
output = {'oracle_qt': qVersion(), 'platform': app.platformName(), 'drawing': drawing_cases, 'undo': undo_cases, 'edit': edit_cases}
# Store repeated reference geometry once. Expectations still come exclusively
# from Python; tests expand these lossless indices before comparing C++ output.
geometry = []; geometry_indices = {}
for case in drawing_cases + edit_cases:
    for step in case['steps']:
        expected = step['expected']
        for key in ['shape', 'path']:
            if key not in expected: continue
            value = expected.pop(key); encoded = json.dumps(value, separators=(',', ':'))
            if encoded not in geometry_indices:
                geometry_indices[encoded] = len(geometry); geometry.append(value)
            expected[key + '_ref'] = geometry_indices[encoded]
output['geometry'] = geometry
(ROOT / arguments.output).write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'drawing_steps': sum(len(c['steps']) for c in drawing_cases), 'undo_steps': sum(map(len, undo_cases)), 'edit_steps': sum(len(c['steps']) for c in edit_cases), 'qt': qVersion()}))
