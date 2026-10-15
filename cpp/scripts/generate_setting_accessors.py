"""Translate only exact, side-effect-free QSettings accessor AST patterns.

All unsupported functions remain pending; never approximate conditional bodies.
"""
from __future__ import annotations
import ast
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'main/settings/tool_settings.py'
tree = ast.parse((ROOT.parent / SOURCE).read_text(encoding='utf-8-sig'))
owner = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'ToolSettingsManager')
existing = set()
header = (ROOT / 'src/settings/tool_settings.h').read_text(encoding='utf-8')
for n in owner.body:
    if isinstance(n, ast.FunctionDef) and n.name + '(' in header:
        existing.add(n.name)

def expression(node):
    if isinstance(node, ast.Constant):
        if isinstance(node.value, str): return json.dumps(node.value, ensure_ascii=False)
        if isinstance(node.value, bool): return 'true' if node.value else 'false'
        if isinstance(node.value, (int, float)): return repr(node.value)
    if isinstance(node, ast.Subscript) and ast.unparse(node.value) == 'self.APP_DEFAULT_SETTINGS':
        return 'app_defaults_.value(' + expression(node.slice) + ')'
    if isinstance(node, ast.Call) and ast.unparse(node.func) == 'self.APP_DEFAULT_SETTINGS.get':
        args = ', '.join(expression(arg) for arg in node.args)
        return 'app_defaults_.value(' + args + ')'
    raise ValueError('Unsupported expression')

methods = []
code = ['// Generated from frozen Python AST. Regenerate; do not hand edit.']
for node in owner.body:
    if not isinstance(node, ast.FunctionDef) or node.name in existing: continue
    body = [n for n in node.body if not (isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant) and isinstance(n.value.value, str))]
    if len(body) != 1: continue
    names = [a.arg for a in node.args.args]
    stmt = body[0]
    try:
        if isinstance(stmt, ast.Return) and len(names) == 1 and isinstance(stmt.value, ast.Call) and ast.unparse(stmt.value.func) == 'self.qsettings.value':
            call = stmt.value
            if len(call.args) != 2: continue
            types = {k.arg: ast.unparse(k.value) for k in call.keywords}
            kind = types.get('type')
            cpp_type, conversion = {'str': ('QString', 'toString'), 'bool': ('bool', 'toBool'),
                                    'int': ('qint64', 'toLongLong'), 'float': ('double', 'toDouble')}[kind]
            path, fallback = map(expression, call.args)
            line = f'{cpp_type} {node.name}() const {{ return qsettings.value({path}, {fallback}).{conversion}(); }}'
            methods.append({'symbol': 'ToolSettingsManager.' + node.name, 'line': node.lineno, 'kind': 'get',
                            'name': node.name, 'path': ast.literal_eval(call.args[0]), 'type': kind})
        elif isinstance(stmt, ast.Return) and len(names) == 1 and isinstance(stmt.value, ast.Subscript) and ast.unparse(stmt.value.value) == 'self.APP_DEFAULT_SETTINGS':
            key = ast.literal_eval(stmt.value.slice)
            line = f'QVariant {node.name}() const {{ return app_defaults_.value({json.dumps(key)}); }}'
            methods.append({'symbol': 'ToolSettingsManager.' + node.name, 'line': node.lineno,
                            'kind': 'get', 'name': node.name, 'path': None, 'type': 'variant'})
        elif isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call) and len(names) == 2 and ast.unparse(stmt.value.func) == 'self.qsettings.setValue':
            call = stmt.value
            if len(call.args) != 2 or not isinstance(call.args[1], ast.Name) or call.args[1].id != names[1]: continue
            path = expression(call.args[0])
            line = f'void {node.name}(const QVariant &value) {{ qsettings.setValue({path}, value); }}'
            methods.append({'symbol': 'ToolSettingsManager.' + node.name, 'line': node.lineno, 'kind': 'set',
                            'name': node.name, 'path': ast.literal_eval(call.args[0])})
        else: continue
    except (ValueError, KeyError, TypeError):
        continue
    code += [f'// {SOURCE}:{node.lineno}', line]

code += ['QVariantMap generated_accessor_values() const {', '    QVariantMap result;']
for method in methods:
    if method['kind'] == 'get':
        code.append(f'    result[{json.dumps(method["name"])}] = {method["name"]}();')
code += ['    return result;', '}']
# One getter by name, like the reference's getattr: calling every getter reads every key, which
# the settings export records as keys the pages read.
code += ['std::optional<QVariant> generated_accessor_value(const QString &name) const {']
for method in methods:
    if method['kind'] == 'get':
        code.append(f'    if (name == {json.dumps(method["name"])}) return QVariant({method["name"]}());')
code += ['    return std::nullopt;', '}']
code += ['bool apply_generated_setter(const QString &name, const QVariant &value) {']
for method in methods:
    if method['kind'] == 'set':
        code.append(f'    if (name == {json.dumps(method["name"])}) {{ {method["name"]}(value); return true; }}')
code += ['    return false;', '}']
(ROOT / 'src/settings/settings_accessors.inc').write_text('\n'.join(code) + '\n', encoding='utf-8')
(ROOT / 'migration/settings_accessor_map.json').write_text(json.dumps(methods, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'generated_exact_accessors': len(methods)}))
