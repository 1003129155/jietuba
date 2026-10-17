"""Developer-only oracle for the clipboard's pure logic and its context menus.

Run with the reference interpreter:
    ..\\venv311\\Scripts\\python.exe -I scripts\\generate_clipboard_fixtures.py

Settings and the clipboard database live in a temporary folder; nothing of the
reference project or the user's data is touched.
"""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import json  # noqa: E402
import os  # noqa: E402
import shutil  # noqa: E402
import tempfile  # noqa: E402
from datetime import datetime  # noqa: E402
from pathlib import Path  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'main'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from PySide6.QtCore import QSettings  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

app = QApplication([])
TMP = Path(tempfile.mkdtemp(prefix='jietuba-clipboard-fixtures-'))

from settings import tool_settings  # noqa: E402

tool_settings._tool_settings_manager = None
config = tool_settings.get_tool_settings_manager(QSettings(str(TMP / 'settings.ini'), QSettings.Format.IniFormat))

from clipboard.core import text_transform  # noqa: E402
from clipboard.services import file_payload_service, group_service, import_export_service  # noqa: E402
from clipboard.ui.panels.setting_panel import escape_separator, unescape_separator  # noqa: E402
from clipboard.ui.image_item_actions import format_from_save_filter  # noqa: E402
from clipboard.ui import layout_scale  # noqa: E402
from clipboard.controllers import clipboard_controller as controller_module  # noqa: E402
from core.ui_scale import get_dialog_scale, get_ui_scale  # noqa: E402
from settings import special_paste  # noqa: E402

TEXTS = [
    '',
    'hello world',
    'Hello World. this is fine! really? yes.',
    "it's a TEST of mixed CASE and o'neil",
    'ÄÖÜ äöü ß straße İstanbul ǅ ǆ ﬁ',
    'Ελληνικά σίσυφος ΣΊΣΥΦΟΣ',
    '中文 文本。second sentence. 第三句！',
    '  leading and trailing  \n',
    'line1\nline2\r\nline3\rline4\n\n\nline5',
    'tabs\tand\u3000full width\u00a0nbsp\u2028sep\x1c\x1d\x1e\x1f',
    '123abc456DEF ٣٤٥ ①②③ ½ ²',
    'a,b;c d\te\nf',
    "x'y, 'quoted', ''",
    '  , ;  ,,',
    '...!!!??? .a !b ?c',
    'abc.def',
    "they're bill's friends from the UK",
    'ﬀ ŉ ΐ',
    '🙂 emoji 😀 text',
]

SQL_CASES = [
    ('hello world  foo   bar', 80),
    ('a,b,c', 80),
    ('1\n2\n3', 80),
    (' '.join(f'value{i}' for i in range(40)), 80),
    (' '.join(f'v{i}' for i in range(30)), 20),
    ('averyveryveryverylongsinglevalue short another', 10),
    ("o'neil;d'arcy", 5),
    ('', 80),
    ('  ,;  ', 80),
    ('中文 日本語 한국어', 8),
    (' '.join(['x'] * 50), 1),
]


def menu_json(actions):
    return [
        {
            'label': action.label,
            'key': action.key,
            'enabled': action.enabled,
            'checkable': action.checkable,
            'checked': action.checked,
            'separator': action.is_separator,
            'translate': action.translate_label,
            'children': menu_json(action.children),
        }
        for action in actions
    ]


def transforms():
    cases = []
    for key, function in text_transform.TRANSFORM_REGISTRY.items():
        if key == 'transform_append_time':
            continue
        for text in TEXTS:
            cases.append({'key': key, 'input': text, 'expected': function(text)})
    return cases


def append_time_cases():
    fixed = datetime(2026, 10, 13, 8, 9, 10)

    class FixedDateTime(datetime):
        @classmethod
        def now(cls, tz=None):
            return fixed

    saved = text_transform.datetime
    text_transform.datetime = FixedDateTime
    try:
        return {
            'now': fixed.isoformat(),
            'cases': [{'input': text, 'expected': text_transform.append_current_time(text)} for text in TEXTS + ['ends space ', 'ends tab\t', 'ends newline\n']],
        }
    finally:
        text_transform.datetime = saved


def file_payload():
    paths = ['C:\\Users\\me\\a.txt', '  C:/Users/me/../me/b.txt  ', 'relative\\.\\dir\\', 'D:', '\\\\server\\share\\x', 'a/b/../../..', '', '中文\\文件.pdf']
    contents = [None, '', '   ', 'C:/raw/path.txt', '{"files": ["C:/x/y.txt", "C:/z"]}', '{"files": []}', '[1, 2]', '{"other": 1}', '"just a string"', '{"files": ["  ./a/../b  "]}']
    return {
        'normalize': [{'input': p, 'expected': file_payload_service.normalize_file_path(p)} for p in paths],
        'build': [{'input': p, 'expected': file_payload_service.build_file_payload(p)} for p in paths],
        'extract': [{'input': c, 'expected': file_payload_service.extract_first_file_path_from_content(c)} for c in contents],
    }


def groups():
    from clipboard.core import GroupType, filter_content_type, filter_group_type, group_accepts

    types = [int(t) for t in GroupType]
    filter_icons = [group_service.get_default_group_icon(t) for t in types[3:]]
    icons = [group_service.GENERAL_GROUP_ICON, group_service.QUICK_LAUNCH_GROUP_ICON, group_service.HIDDEN_GROUP_ICON, '⭐', ''] + filter_icons
    content_types = ['text', 'image', 'file']

    class G:
        def __init__(self, gid, name):
            self.id, self.name = gid, name

    existing = [G(1, 'Work'), G(2, 'Apps'), G(3, 'work')]
    return {
        'icons': {
            'general': group_service.GENERAL_GROUP_ICON,
            'quick_launch': group_service.QUICK_LAUNCH_GROUP_ICON,
            'hidden': group_service.HIDDEN_GROUP_ICON,
        },
        'default': [{'type': t, 'expected': group_service.get_default_group_icon(t)} for t in types],
        'display': [
            {'icon': icon or None, 'type': t, 'expected': group_service.get_group_display_icon(icon or None, t)}
            for icon in icons for t in types
        ],
        'toggled': [
            {'icon': icon, 'type': t, 'expected': group_service.get_toggled_default_group_icon(icon, t)}
            for icon in icons for t in types
        ],
        'filter_content_type': [{'type': t, 'expected': filter_content_type(t)} for t in types + [-1, 6, 99]],
        'filter_group_type': [{'content_type': c, 'expected': int(filter_group_type(c))} for c in content_types],
        'accepts': [{'type': t, 'content_type': c, 'expected': group_accepts(t, c)} for t in types + [99] for c in content_types],
        'name_exists': [
            {'name': name, 'exclude': exclude, 'expected': group_service.group_name_exists(existing, name, exclude)}
            for name in ('Work', 'work', 'Apps', 'Other', '') for exclude in (None, 1, 2, 3)
        ],
        'existing': [[g.id, g.name] for g in existing],
        'unique': [
            {'base': base, 'used': sorted(used), 'expected': group_service.make_unique_group_name(base, used)}
            for base, used in [
                ('New', set()),
                ('New', {'New'}),
                ('New', {'New', 'New (1)', 'New (2)'}),
                ('New', {'New', 'New (2)'}),
                ('分组', {'分组', '分组 (1)'}),
            ]
        ],
        'delete_message': group_service.build_delete_group_confirm_message(lambda text: f'<{text}>'),
    }


def special_paste_cases():
    inputs = [
        None,
        [],
        'not json',
        '{"a": 1}',
        json.dumps([['transform_lowercase', False], ['transform_uppercase', True]]),
        json.dumps([['unknown_key', True], ['transform_sql_in', 0], ['transform_sql_in', True], ['file_name', 'yes']]),
        json.dumps([['transform_uppercase'], 'transform_lowercase', ['file_path', True, 'extra'], [1, True], ['file_name', 1]]),
        json.dumps([[key, index % 3 != 0] for index, key in enumerate(reversed(list(special_paste.OPTIONS_BY_KEY)))]),
        [['file_parent_path', False], ['transform_remove_whitespace', True]],
    ]
    # Text is what the setting stores, so it goes through load(); other values straight to normalize().
    normalized, loaded = [], []
    for value in inputs:
        if isinstance(value, str):
            config.set_app_setting(special_paste.SETTING_KEY, value)
            loaded.append({'raw': value, 'expected': [list(entry) for entry in special_paste.load(config)]})
        else:
            normalized.append({'input': value, 'expected': [list(entry) for entry in special_paste.normalize(value)]})
    config.set_app_setting(special_paste.SETTING_KEY, '')
    loaded.append({'raw': '', 'expected': [list(entry) for entry in special_paste.load(config)]})
    return {
        'options': [
            {'key': o.key, 'label': o.label, 'description': o.description, 'content_type': o.content_type, 'group': o.group, 'enabled': o.enabled}
            for o in special_paste.BUILTIN_OPTIONS
        ],
        'defaults': [list(entry) for entry in special_paste.default_entries()],
        'normalize': normalized,
        'load': loaded,
        'saved': _saved_text(special_paste.normalize(inputs[7])),
    }


def _saved_text(entries):
    special_paste.save(config, entries)
    return config.get_app_setting(special_paste.SETTING_KEY)


def separators():
    texts = ['', '\n', '\t', '\\', '\\n', 'a\nb\tc\\d', '\\\\n', '\\t\\x', 'trailing\\', ', ', '中\n文']
    return [{'text': t, 'escaped': escape_separator(t), 'unescaped': unescape_separator(t)} for t in texts]


def time_ranges():
    texts = [
        '', '   ', '2026/06/20-2026/06/30', '2026-6-1 ~ 2026-6-1', '2026.02.28至2026.03.01', '2024/02/29到2024/03/01',
        '2026/02/29-2026/03/01', '2026/06/30-2026/06/20', '2026/13/01-2026/13/02', ' 2026/1/1 - 2027/1/1 ', '2026/01/01', '26/01/01-26/01/02',
        '2026/01/01—2026/01/02', '2026/1/1-2026/1/31x',
    ]
    result = []
    for text in texts:
        parsed = controller_module.ClipboardController._parse_time_range_text(text)
        result.append({'text': text, 'expected': None if parsed is None else [parsed[0].isoformat(), parsed[1].isoformat()]})
    return result


def capacities():
    result = []
    for percent in (100, 125, 150, 80):
        get_ui_scale().set_percent(percent)
        sizes = [-5, 0, 1, 50, 100, 197, 198, 235, 236, 300, 600, 1080]
        result.append({
            'percent': percent,
            'sidebar': [[size, controller_module.calc_sidebar_capacity(size)] for size in sizes],
            'topbar': [[size, controller_module.calc_topbar_capacity(size)] for size in sizes],
        })
    get_ui_scale().set_percent(100)
    return result


def dialog_sizes():
    result = []
    for percent in (100, 125, 80, 200):
        get_dialog_scale().set_percent(percent)
        result.append({
            'percent': percent,
            'width': layout_scale.manage_dialog_width(),
            'height': layout_scale.manage_dialog_height(),
            'min_width': layout_scale.manage_dialog_min_width(),
            'min_height': layout_scale.manage_dialog_min_height(),
            'fit': [[w, h, *layout_scale.fit_manage_dialog_size(w, h)] for w, h in [(1920, 1040), (1366, 728), (800, 600), (2560, 1400), (1000, 700)]],
        })
    get_dialog_scale().set_percent(100)
    return result


def save_filters():
    cases = [
        ('C:/a/b.png', 'PNG (*.png)'), ('C:/a/b', 'JPEG (*.jpg *.jpeg)'), ('C:/a/b', 'All files (*)'), ('C:/a/b.tar.gz', ''),
        ('C:/a.b/c', 'BMP (*.bmp)'), ('C:/a/b.', 'WebP (*.webp)'), ('C:/a/.hidden', ''), ('C:/a/b', '*.tiff'),
    ]
    return [{'path': p, 'filter': f, 'expected': format_from_save_filter(p, f)} for p, f in cases]


def csv_cases():
    import csv
    import io

    rows = [
        ['group', 'content', 'title'],
        ['a', 'plain', ''],
        ['comma,inside', 'quote "inside"', 'new\nline'],
        ['', ' spaces ', '\r\nwin'],
        ['中文', 'tab\tsep', 'x'],
    ]
    formatted = []
    for row in rows:
        buffer = io.StringIO(newline='')
        csv.writer(buffer).writerow(row)
        formatted.append({'row': row, 'expected': buffer.getvalue()})
    contents = [
        'group,content,title\r\na,b,c\r\n',
        'h1,h2\n"x,y","multi\nline",z\nonly-one\n,empty-group\n g ,c, t \n',
        'h\n"a ""quoted"" b",c\n"unterminated,d\n',
        '',
        'h1,h2,h3\r\nx,"",t\r\ny,v\r\n\r\nz,w,extra,more\r\n',
    ]
    parsed = [{'content': c, 'expected': [list(r) for r in csv.reader(io.StringIO(c))]} for c in contents]
    imported = []
    for index, content in enumerate(contents):
        path = TMP / f'import_{index}.csv'
        path.write_text(content, encoding='utf-8', newline='')
        imported.append({'content': content, 'expected': [list(r) for r in import_export_service.read_import_rows(str(path))]})
    return {'format': formatted, 'parse': parsed, 'import': imported, **csv_encoding_cases()}


def csv_encoding_cases():
    """CSV export in each offered encoding, and imports of files saved in the encodings users meet."""
    import base64

    header = ['分组', 'Content', 'タイトル']
    rows = [
        ['日本語', '10時～12時 〜 ① 髙 ‖ － ￠ ￢', 'メモ'],
        ['中文', '中文测试 €100 — “引号”', '标题'],
        ['Latin', 'naïve café ß', ''],
        ['Emoji', '😀 👍', 'x'],
    ]
    exported = []
    for index, encoding in enumerate(['utf-8-sig', 'utf-8', 'shift_jis', 'gbk']):
        path = TMP / f'export_{index}.csv'
        import_export_service.write_csv_rows(str(path), header, rows, encoding)
        exported.append({'encoding': encoding, 'bytes': base64.b64encode(path.read_bytes()).decode('ascii')})

    def body(text, encoding):
        return ('group,content,title\r\n' + text).encode(encoding, errors='replace')

    files = [
        body('メモ,10時〜12時 ～ ‖ − ￠ ￢,題\r\n', 'shift_jis'),
        body('メモ,① ② 髙 﨑,題\r\n', 'cp932'),
        body('笔记,中文测试 ～ ‖ －,标题\r\n', 'gbk'),
        body('笔记,中文 € 测试,标题\r\n', 'gb18030'),
        body('notes,naïve café € ‚ „,t\r\n', 'cp1252'),
        b'group,content,title\r\nraw,\x81\x8d\x8f\x90\x9d \xe9,t\r\n',
        b'\xef\xbb\xbfgroup,content\r\nbom,\xe4\xb8\xad\r\n',
        b'group,content\r\nbad,\xff\xfe\xfa\r\n',
        # Excel's ANSI save on a Chinese system: cp936, where € is the single byte 0x80.
        b'group,content,title\r\n' + '报价,单价 '.encode('gbk') + b'\x80' + '5，含税,标题\r\n'.encode('gbk'),
    ]
    imported = []
    for index, data in enumerate(files):
        path = TMP / f'import_encoded_{index}.csv'
        path.write_bytes(data)
        imported.append({'bytes': base64.b64encode(data).decode('ascii'), 'expected': [list(r) for r in import_export_service.read_import_rows(str(path))]})
    return {'header': header, 'rows': rows, 'export_encoded': exported, 'import_encoded': imported}


def menus():
    from clipboard.core import ClipboardManager, GroupType
    from clipboard.controllers.clipboard_controller import ClipboardController

    manager = ClipboardManager(str(TMP / 'clipboard' / 'clipboard.db'))
    assert manager.is_available
    group_specs = [
        ('工作', '📁', int(GroupType.NORMAL)), ('Apps', '⚡', int(GroupType.FILE)), ('私密', '🙈', int(GroupType.HIDDEN)), ('Notes', None, int(GroupType.NORMAL)),
        ('只看文本', None, int(GroupType.FILTER_TEXT)), ('只看图片', '🖼️', int(GroupType.FILTER_IMAGE)),
    ]
    group_ids = [manager.create_group(name, None, icon, group_type) for name, icon, group_type in group_specs]
    file_payload_text = file_payload_service.build_file_payload('C:\\data\\report.pdf')
    item_specs = [
        ('hello world', 'text', None, False, None),
        ('pinned text', 'text', 'A title', True, None),
        ('image-content', 'image', None, False, None),
        (file_payload_text, 'file', None, False, None),
        ('in group A', 'text', None, False, 0),
        ('second in A', 'text', 'kept title', False, 0),
        (file_payload_text, 'file', None, False, 1),
        ('third in A', 'text', None, False, 0),
    ]
    item_ids = []
    for content, content_type, title, pinned, group_index in item_specs:
        item_id = manager.add_item(content, content_type, title=title)
        assert item_id
        if pinned:
            manager.toggle_pin(item_id)
        if group_index is not None:
            manager.move_to_group(item_id, group_ids[group_index])
        item_ids.append(item_id)

    def tokens(value):
        if isinstance(value, list):
            return [tokens(v) for v in value]
        if isinstance(value, dict):
            return {k: tokens(v) for k, v in value.items()}
        if isinstance(value, str) and value.startswith('move_to_group_'):
            return 'move_to_group_{G%d}' % group_ids.index(int(value[len('move_to_group_'):]))
        return value

    configs = {
        'default': special_paste.default_entries(),
        'custom': special_paste.normalize([
            ['transform_lowercase', True], ['transform_uppercase', False], ['transform_sql_in', True],
            ['file_name', True], ['file_path', False],
        ]),
        'text_off': [(key, False if option.content_type == 'text' else True) for key, option in special_paste.OPTIONS_BY_KEY.items()],
    }
    queries = []
    for config_name, entries in configs.items():
        special_paste.save(config, entries)
        controller = ClipboardController(manager)
        for group_index in (None, 0, 1, 3, 4, 5):
            controller.switch_to_group(None if group_index is None else group_ids[group_index])
            controller.current_group_id = None if group_index is None else group_ids[group_index]
            controller.load_history()
            for item_index in range(len(item_ids)):
                data = controller.build_context_menu_data(item_ids[item_index])
                queries.append({
                    'special_paste': config_name, 'group': group_index, 'kind': 'item', 'items': [item_index],
                    'expected': None if data is None else tokens(menu_json(data.actions)),
                })
            if config_name != 'default':
                continue
            for marked in ([0, 2], [2], [3], [0, 3], [4, 5], [6], [1, 2, 3], [99]):
                ids = [item_ids[i] if i < len(item_ids) else 999999 for i in marked]
                data = controller.build_marked_context_menu_data(ids)
                queries.append({
                    'special_paste': config_name, 'group': group_index, 'kind': 'marked', 'items': marked,
                    'expected': None if data is None else {'item': item_ids.index(data.item_id), 'actions': tokens(menu_json(data.actions))},
                })
        if config_name == 'default':
            for group_index in range(len(group_ids)):
                queries.append({
                    'special_paste': config_name, 'group': group_index, 'kind': 'group', 'items': [],
                    'expected': tokens(menu_json(controller.build_group_context_menu_data(group_ids[group_index]))),
                })
        controller.deleteLater()
    return {
        'groups': [{'name': n, 'icon': i, 'type': t} for n, i, t in group_specs],
        'items': [{'content': c, 'type': t, 'title': ti, 'pinned': p, 'group': g} for c, t, ti, p, g in item_specs],
        'configs': {name: [list(entry) for entry in entries] for name, entries in configs.items()},
        'queries': queries,
    }


def py_text_cases():
    import ntpath

    texts = TEXTS + ['\u3000 full width \u3000', '\x1c\x1dsep\x1e\x1f', 'xxhixx', '']
    paths = [
        'C:\\a\\b\\c.txt', 'C:/a/b/../c/./d.txt', 'C:', 'C:\\', 'C:relative\\x', '\\\\server\\share\\dir\\f.txt', '\\\\server\\share',
        '\\\\?\\UNC\\srv\\share\\x', '//server/share/x/', 'a\\\\b//c\\', '..\\..\\x', '.', '', 'D:\\x\\..\\..\\y', '/abs/path', '\\lead',
        'name.txt', 'dir\\', 'C:\\Dir\\Sub', 'c:\\dir\\SUB', '\u4e2d\u6587\\\u6587\u4ef6.pdf',
    ]
    joins = [
        ('C:\\a', 'b'), ('C:\\a\\', 'b'), ('C:\\a', '\\b'), ('C:\\a', 'D:\\b'), ('C:\\a', 'c:b'), ('C:\\a', 'D:b'), ('', 'x'), ('a', ''),
        ('\\\\srv\\share', 'x'), ('C:', 'x'), ('a/', 'b'), ('C:\\a', 'C:'),
    ]
    return {
        'strings': [
            {
                'text': t, 'len': len(t), 'strip': t.strip(), 'lstrip': t.lstrip(), 'rstrip': t.rstrip(), 'strip_x': t.strip('x'),
                'upper': t.upper(), 'lower': t.lower(), 'title': t.title(), 'swapcase': t.swapcase(), 'casefold': t.casefold(),
                'slice_2_5': t[2:5], 'slice_neg': t[-3:], 'slice_neg_end': t[:-2],
            }
            for t in texts
        ],
        'isspace': [[code, chr(code).isspace()] for code in list(range(0, 0x30)) + [0x85, 0xA0, 0x1680, 0x2000, 0x200A, 0x200B, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF]],
        'paths': [
            {'path': x, 'basename': ntpath.basename(x), 'dirname': ntpath.dirname(x), 'normpath': ntpath.normpath(x), 'normcase': ntpath.normcase(x)}
            for x in paths
        ],
        'joins': [{'a': a, 'b': b, 'expected': ntpath.join(a, b)} for a, b in joins],
    }


fixture = {
    'py_text': py_text_cases(),
    'transforms': transforms(),
    'sql_in': [{'input': text, 'max': width, 'expected': text_transform.to_sql_in_clause(text, width)} for text, width in SQL_CASES],
    'append_time': append_time_cases(),
    'file_payload': file_payload(),
    'groups': groups(),
    'special_paste': special_paste_cases(),
    'separators': separators(),
    'time_ranges': time_ranges(),
    'capacities': capacities(),
    'dialog_sizes': dialog_sizes(),
    'save_filters': save_filters(),
    'csv': csv_cases(),
    'menus': menus(),
}
target = ROOT / 'tests' / 'fixtures' / 'clipboard.json'
target.write_text(json.dumps(fixture, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
app.processEvents()
shutil.rmtree(TMP, ignore_errors=True)
print(f'wrote {target}')
