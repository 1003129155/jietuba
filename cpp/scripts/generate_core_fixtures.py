"""Developer-only oracle for core/logger, core/constants, core/i18n, core/global_hotkey and core/elevation.

Run with the reference interpreter:
    ..\\venv311\\Scripts\\python.exe scripts\\generate_core_fixtures.py
"""
from __future__ import annotations

import json
import os
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'main'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from core.logger import T, LogLevel  # noqa: E402
from core.i18n import I18nManager  # noqa: E402
from core.log_translations import TRANSLATIONS  # noqa: E402
from core import global_hotkey  # noqa: E402
from core import elevation  # noqa: E402

random.seed(20261013)
# -0.0 is omitted: QJsonValue reads it back as integer 0.
floats = [0.0, 1.0, 0.1, 1 / 3, 2.5, 1e16, 1e15, 123456789.0, 1e-4, 1e-5, 12.345, -7.25, 1.5e300, 5e-324]
floats += [random.uniform(-1e6, 1e6) for _ in range(40)]
floats += [random.uniform(-1, 1) * 10 ** random.randint(-8, 20) for _ in range(40)]

render_cases = []
templates = sorted(TRANSLATIONS)[::17]
for language in ('zh', 'en', 'ja'):
    I18nManager._current_lang = language
    for index, template in enumerate(templates):
        import string
        names = [f[1] for f in string.Formatter().parse(template) if f[1]]
        args = {name: (index * 7 + n if n % 2 else f'v{n}') for n, name in enumerate(names)}
        try:
            rendered = T(template, **args).render()
        except Exception:
            continue
        render_cases.append({'language': language, 'template': template, 'args': args, 'expected': rendered})
    for template, args in [
        ('比例 {ratio:.1f}', {'ratio': 2.25}),
        ('值 {a:.3f} / {b:.0f}', {'a': 1.23456, 'b': 2.5}),
        ('缺参数 {missing}', {'other': 1}),
        ('转义 {{literal}} {x}', {'x': True}),
        ('浮点 {x}', {'x': 0.1}),
    ]:
        try:
            expected = T(template, **args).render()
        except Exception as e:
            expected = None
        render_cases.append({'language': language, 'template': template, 'args': args, 'expected': expected})

hotkey_texts = ['', '   ', 'ctrl+', 'ctrl++', '+a', 'a+b', 'ctrl', 'Ctrl + Shift + A', 'mouseback', ' MouseForward ',
                'mousemiddle', 'ctrl+!', 'f25', 'control+meta+x', 'super+1', 'win+win+v', 'ctrl+alt+shift+del']
for prefix in ('', 'ctrl+', 'shift+', 'alt+', 'win+', 'ctrl+alt+', 'shift+win+', 'ctrl+shift+alt+win+'):
    hotkey_texts += [prefix + name for name in sorted(global_hotkey._NAME_TO_VK)]
hotkey_routes = []
for text in hotkey_texts:
    try:
        parsed = list(global_hotkey.parse_hotkey(text))
    except ValueError:
        parsed = None
    hotkey_routes.append({'text': text, 'route': global_hotkey.route(text), 'parsed': parsed})
hotkey_formats = [{'mods': mods, 'vk': vk, 'expected': global_hotkey.format_hotkey(mods, vk)}
                  for mods in (0, 1, 2, 4, 8, 15, 0x4008) for vk in range(256)]
hotkey_route_of = [{'mods': mods, 'vk': vk, 'expected': global_hotkey.route_of(mods, vk)}
                   for mods in (0, 0x4000, 8, 0x4008, 3, 0x4003, 7, 12) for vk in range(256)]

fixture = {
    'float_repr': [{'value': value, 'expected': repr(value)} for value in floats],
    'global_hotkey': {
        'route': hotkey_routes,
        'format': hotkey_formats,
        'route_of': hotkey_route_of,
        'modifier_names': {str(mods): global_hotkey.modifier_names(mods) for mods in range(16)},
    },
    'elevation_task_xml': [
        {'command': command, 'user': user, 'expected': elevation.build_task_xml(command, user)}
        for command, user in ((r'C:\Apps\Jietuba\jietuba.exe', r'PC\user'),
                              (r'D:\A & B <x>\jietuba_lite.exe', r'DOMAIN\Zoë'),
                              (r'C:\jietuba.exe', r'PC\user'))
    ],
    'level_names': {str(level): LogLevel.name(level) for level in (10, 20, 30, 40, 50, 0, 99)},
    'render': render_cases,
}
target = ROOT / 'tests' / 'fixtures' / 'core.json'
target.write_text(json.dumps(fixture, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
print(f"{len(fixture['float_repr'])} floats, {len(render_cases)} renders -> {target.relative_to(ROOT)}")
