"""Execute frozen Python functions to generate independent C++ parity oracles."""
from __future__ import annotations
import importlib.util
import json
import os
import random
import sys
import tempfile
import hashlib
from pathlib import Path

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT.parent / 'main'
sys.path.insert(0, str(REFERENCE))
# The log folder depends on the machine; the C++ tests compare it with their own.
LOG_DIR_PLACEHOLDER = '${LOCALAPPDATA}/Jietuba/Logs'

def load(name, relative):
    spec = importlib.util.spec_from_file_location('_reference_' + name, REFERENCE / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module

from PySide6.QtCore import QRectF, QSettings, qVersion, Qt
from PySide6.QtGui import QImage, QPixmap, QColor, QPen, QBrush
from PySide6.QtWidgets import QApplication, QGraphicsScene
app = QApplication([])
selection = load('selection', 'canvas/selection_model.py')
scale = load('scale', 'core/ui_scale.py')
layout = load('layout', 'ui/toolbar_layout.py')
settings = load('settings', 'settings/tool_settings.py')
exports = load('exports', 'core/export.py')
colors = load('colors', 'settings/color_formats.py')
saves = load('saves', 'core/save.py')

def rect(r):
    return [r.x(), r.y(), r.width(), r.height()]

def selection_oracle(actions):
    model = selection.SelectionModel()
    events = []
    def event(name, value):
        events.append({'signal': name, 'value': value, 'active': model.is_active(),
                       'confirmed': model.is_confirmed, 'dragging': model.is_dragging})
    model.rectChanged.connect(lambda r: event('rect', rect(r)))
    model.confirmed.connect(lambda r: event('confirmed', rect(r)))
    model.draggingChanged.connect(lambda v: event('dragging', v))
    steps = []
    for action in actions:
        before = len(events)
        caller = None
        if action['op'] == 'set':
            value = QRectF(*action['rect'])
            model.set_rect(value)
            caller = rect(value)
        elif action['op'] == 'initialize':
            model.initialize_confirmed_rect(QRectF(*action['rect']))
        elif action['op'] == 'mutate_copy':
            value = model.rect()
            value.setWidth(999)
        else:
            getattr(model, action['op'])()
        steps.append({'action': action, 'caller': caller, 'rect': rect(model.rect()),
                      'active': model.is_active(), 'confirmed': model.is_confirmed,
                      'dragging': model.is_dragging, 'empty': model.is_empty(), 'events': events[before:]})
    return steps

rng = random.Random(20261012)
selection_cases = [selection_oracle([
    {'op': 'confirm'}, {'op': 'activate'}, {'op': 'set', 'rect': [10, 20, 2, 3]},
    {'op': 'set', 'rect': [10, 20, 8, 8]}, {'op': 'start_dragging'}, {'op': 'start_dragging'},
    {'op': 'confirm'}, {'op': 'deactivate'}, {'op': 'stop_dragging'}, {'op': 'mutate_copy'},
    {'op': 'initialize', 'rect': [-300, -100, 1, 1]}, {'op': 'normalize'}])]
for _ in range(64):
    actions = []
    for _ in range(24):
        op = rng.choice(['set', 'initialize', 'activate', 'deactivate', 'confirm', 'normalize',
                         'start_dragging', 'stop_dragging', 'mutate_copy'])
        action = {'op': op}
        if op in ('set', 'initialize'):
            action['rect'] = [rng.uniform(-500, 500), rng.uniform(-500, 500),
                              rng.choice([0, 1, 7, 8, -20, 50, 100.5]), rng.choice([0, 1, 7, 8, -20, 50, 100.5])]
        actions.append(action)
    selection_cases.append(selection_oracle(actions))

layout_inputs = [None, [], 'garbage', 3, {'pen': 'hide'},
                 [['mosaic', 'hide'], ['nope', 'show'], ['mosaic', 'show'], 'xy', 3,
                  ['save', 'weird'], ['scan_code', 'weird']], [['confirm', 'hide']]]
for _ in range(256):
    order = list(layout.DEFAULT_ORDER)
    rng.shuffle(order)
    inputs = [[key, rng.choice(['show', 'hide', 'more', 'weird', None, 3])] for key in order[:rng.randrange(len(order) + 1)]]
    inputs += rng.sample(inputs, min(len(inputs), 3)) + [['unknown', 'show'], [], ['pen'], ['pen', 'hide', 'extra']]
    layout_inputs.append(inputs)
layout_inputs += [[{'pen': 'unused', 'hide': 0}], [{'confirm': {}, 'hide': 0}],
                  [{'hide': 'unused', 'pen': 0}], [{'scan_code': [3, 4], 'more': 'ignored'}]]
layout_cases = [{'input': value, 'input_json': json.dumps(value), 'expected': layout.normalize_layout(value)} for value in layout_inputs]
# Pin profile: no locked button, capture-only keys are unknown. Its own generator keeps the other sequences unchanged.
pin_rng = random.Random(20261013)
pin_layout_inputs = [None, [], [['confirm', 'hide'], ['copy', 'weird'], ['long_screenshot', 'show'], ['watermark', 'show']]]
for _ in range(64):
    order = list(layout.PIN.order) + ['confirm', 'long_screenshot', 'pin']
    pin_rng.shuffle(order)
    inputs = [[key, pin_rng.choice(['show', 'hide', 'more', 'weird', None])] for key in order[:pin_rng.randrange(len(order) + 1)]]
    inputs += pin_rng.sample(inputs, min(len(inputs), 2))
    pin_layout_inputs.append(inputs)
pin_layout_cases = [{'input_json': json.dumps(value), 'expected': layout.normalize_layout(value, layout.PIN)} for value in pin_layout_inputs]

scale_inputs = [None, 'abc', True, False, '', [], {}, 0, 999, -1, 97, 118, 117.5, 162.5, '125.5']
scale_inputs += [v / 2 for v in range(120, 451)]
scale_cases = [{'input': v, 'expected': scale.UIScaleManager.normalize_percent(v)} for v in scale_inputs]
pixel_cases = []
for percent in scale.UIScaleManager.PERCENT_OPTIONS:
    manager = scale.UIScaleManager()
    manager.set_percent(percent)
    for base in [0, 1, -1, 2.5, -2.5, .1, -.1, 3, -3, 17, 200] + [v / 2 for v in range(-40, 41)]:
        pixel_cases.append({'percent': percent, 'base': base, 'px': manager.px(base), 'pxf': manager.pxf(base)})
dpis = [72, 96, 97, 120, 144, 145, 168, 0, -1, 'invalid', 'nan', 'inf', False, True, [], {}]
dpi_cases = [{'input': v, 'expected': scale.recommended_scale_percent(v)} for v in dpis]

default_registry = {'app': settings.ToolSettingsManager.APP_DEFAULT_SETTINGS,
                    'tools': settings.ToolSettingsManager.DEFAULT_SETTINGS}
default_registry['tool_order'] = list(default_registry['tools'])
default_registry['app_types'] = {k: type(v).__name__ for k, v in default_registry['app'].items()}
default_registry['tool_types'] = {tool: {k: type(v).__name__ for k, v in values.items()}
                                for tool, values in default_registry['tools'].items()}
default_registry['app'] = dict(default_registry['app'])
default_registry['app']['screenshot_save_path'] = '${HOME}/Pictures/jietuba_photos'
default_registry['app']['log_dir'] = LOG_DIR_PLACEHOLDER
(ROOT / 'resources/defaults.json').write_text(json.dumps(default_registry, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

settings_cases = []
with tempfile.TemporaryDirectory() as temp:
    for index, (tool, key, raw) in enumerate([
        ('pen', 'stroke_width', '7.9'), ('pen', 'stroke_width', ['6.5', '9']),
        ('pen', 'stroke_width', []), ('pen', 'stroke_width', 'bad'),
        ('pen', 'opacity', '0.45'), ('pen', 'opacity', ['0.7']),
        ('text', 'background_enabled', 'yes'), ('text', 'background_enabled', 'true'),
        ('text', 'background_enabled', 'false'), ('text', 'background_enabled', '1'),
        ('text', 'background_enabled', 'on'), ('text', 'background_enabled', ' TRUE '),
        ('text', 'background_enabled', 0), ('text', 'background_enabled', 2),
        ('pen', 'color', []), ('pen', 'color', ['#123456', '#789ABC']),
        ('pen', 'color', True), ('pen', 'stroke_width', False),
    ]):
        qs = QSettings(str(Path(temp) / f'{index}.ini'), QSettings.IniFormat)
        qs.setValue(f'tools/{tool}/{key}', raw)
        model = settings.ToolSettingsManager(qsettings=qs)
        settings_cases.append({'tool': tool, 'key': key, 'raw': raw, 'expected': model.get_setting(tool, key)})
    qs = QSettings(str(Path(temp) / 'all.ini'), QSettings.IniFormat)
    manager = settings.ToolSettingsManager(qsettings=qs)
    app_reads = {key: manager.get_app_setting(key) for key in manager.APP_DEFAULT_SETTINGS}
    app_reads['screenshot_save_path'] = '${HOME}/Pictures/jietuba_photos'
    app_reads['log_dir'] = LOG_DIR_PLACEHOLDER
    accessor_map = json.loads((ROOT / 'migration/settings_accessor_map.json').read_text(encoding='utf-8'))
    accessor_reads = {row['name']: getattr(manager, row['name'])() for row in accessor_map if row['kind'] == 'get'}
    accessor_reads['get_log_dir'] = LOG_DIR_PLACEHOLDER
    accessor_setters = []
    for row in accessor_map:
        if row['kind'] != 'set': continue
        raw = 'migration-probe-value'
        getattr(manager, row['name'])(raw)
        accessor_setters.append({'name': row['name'], 'input': raw, 'path': row['path'], 'stored': qs.value(row['path'])})

export_cases = []
for region in [[-50, -30, 32, 24], [-47, -28, 10, 8], [-48.5, -28.5, 10.9, 8.9],
               [-60, -40, 25, 20], [0, 0, 0, 0], [0, 0, -5, 10], [-49, -29, .5, .5]]:
    scene = QGraphicsScene()
    scene.setSceneRect(QRectF(-50, -30, 32, 24))
    scene.selection_model = selection.SelectionModel()
    image = QImage(32, 24, QImage.Format_ARGB32_Premultiplied)
    for y in range(24):
        for x in range(32):
            image.setPixelColor(x, y, QColor((x * 17) % 256, (y * 23) % 256, (x + y) * 4, 255))
    scene.background = scene.addPixmap(QPixmap.fromImage(image))
    scene.background.setOffset(-50, -30)
    scene.background.setZValue(0)
    drawing = scene.addRect(QRectF(-45, -26, 8, 6), QPen(Qt.NoPen), QBrush(QColor(255, 0, 0, 128)))
    drawing.setZValue(20)
    hidden = scene.addRect(QRectF(-40, -20, 6, 4), QPen(Qt.NoPen), QBrush(Qt.blue))
    hidden.setVisible(False)
    service = exports.ExportService(scene)
    row = {'region': region}
    for name, method in [('composite', service.export), ('base', service.export_base_image_only)]:
        result = method(QRectF(*region))
        row[name] = {'width': result.width(), 'height': result.height(),
                     'sha256': hashlib.sha256(bytes(result.constBits())).hexdigest() if not result.isNull() else ''}
    row['drawing_visible'] = drawing.isVisible()
    row['hidden_visible'] = hidden.isVisible()
    export_cases.append(row)

def color_rows(formats):
    return [{'name': f.name, 'template': f.template, 'enabled': f.enabled} for f in formats]
color_render = []
for rgb in [[0, 0, 0], [255, 255, 255], [128, 128, 128], [255, 0, 0], [0, 255, 0], [0, 0, 255]] + [[rng.randrange(256) for _ in range(3)] for _ in range(128)]:
    color_render.append({'rgb': rgb, 'expected': [f.render(QColor(*rgb)) for f in colors.default_formats()]})
color_loading = []
with tempfile.TemporaryDirectory() as temp:
    for index, (raw, legacy) in enumerate([
        ('', ''), ('', 'hex'), ('', 'rgb_hex'), ('{bad', 'rgb_css'), ('{}', 'hsl_css'),
        ('[]', 'hex_bare'), ('[3, null]', 'rgb'), ('[{"name":"HEX","enabled":false}]', ''),
        ('[{"name":"HEX","enabled":"false"},{"name":"HEX","enabled":false}]', ''),
        ('[{"name":"bogus","enabled":true}]', 'hex'),
    ]):
        qs = QSettings(str(Path(temp) / f'color{index}.ini'), QSettings.IniFormat)
        config = settings.ToolSettingsManager(qsettings=qs)
        config.set_app_setting(colors.SETTING_KEY, raw)
        config.set_app_setting(colors.LEGACY_SETTING_KEY, legacy)
        result = colors.load(config)
        colors.save(config, result)
        color_loading.append({'raw': raw, 'legacy': legacy, 'expected': color_rows(result),
                              'saved': config.get_app_setting(colors.SETTING_KEY)})

save_temp = tempfile.TemporaryDirectory()
save_qs = QSettings(str(Path(save_temp.name) / 'save.ini'), QSettings.IniFormat)
save_config = settings.ToolSettingsManager(qsettings=save_qs)
service = saves.SaveService(save_config)
save_formats = []
for fmt in ['', 'PNG', ' jpeg ', '.jpg', '..webp', 'PDF', ' . ', 'weird']:
    for w, h, dpi in [(100, 200, 300), (65500, 1, 300), (65501, 1, 300),
                      (1, 16383, 300), (1, 16384, 300), (60000, 1, 300), (60001, 1, 300), (14401, 1, 72)]:
        save_formats.append({'format': fmt, 'width': w, 'height': h, 'dpi': dpi,
                             'expected': service.format_for_size(fmt, w, h, pdf_dpi=dpi)})
flatten_cases = []
for alpha in [0, 1, 64, 128, 254, 255]:
    image = QImage(4, 3, QImage.Format_ARGB32)
    image.fill(QColor(255, 40, 80, alpha))
    flattened = service._flatten_for_pdf(image)
    flatten_cases.append({'alpha': alpha, 'sha256': hashlib.sha256(bytes(flattened.constBits())).hexdigest()})

output = {'oracle_qt': qVersion(), 'selection': selection_cases, 'layout': layout_cases, 'pin_layout': pin_layout_cases,
          'scale': scale_cases, 'pixels': pixel_cases, 'dpi': dpi_cases,
          'tool_coercion': settings_cases, 'app_reads': app_reads, 'exports': export_cases,
          'accessor_reads': accessor_reads, 'accessor_setters': accessor_setters,
          'color_render': color_render, 'color_loading': color_loading,
          'save_formats': save_formats, 'pdf_flatten': flatten_cases}
target = ROOT / 'tests/fixtures/parity.json'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(json.dumps(output, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
print(json.dumps({'selection_steps': sum(map(len, selection_cases)), 'layouts': len(layout_cases), 'pin_layouts': len(pin_layout_cases),
                  'scales': len(scale_cases), 'pixels': len(pixel_cases), 'dpis': len(dpi_cases),
                  'tool_coercions': len(settings_cases), 'app_defaults': len(app_reads),
                  'oracle_qt': qVersion()}))
