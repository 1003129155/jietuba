"""Developer-only record of the Python commit the C++ version has caught up with. Never imported by the EXE.

verify lists the Python files changed since then; sync moves the record to a commit once C++ matches it.
"""
from __future__ import annotations
import argparse
import ast
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
MANIFEST = ROOT / 'migration' / 'source_manifest.json'
INVENTORY = ROOT / 'migration' / 'symbol_inventory.json'
# .json brings in svg/emoji_groups.json, the group icon picker's data.
# .lock: the updater is built with the pinned versions of rust_libs.
EXTENSIONS = {'.py', '.rs', '.toml', '.lock', '.svg', '.qss', '.xml', '.qrc', '.ico', '.md', '.txt', '.yml', '.yaml', '.bat', '.json'}
ROOTS = ('main/', 'rust_libs/', 'svg/', 'licenses/')
# requirements.txt pins oneocr, whose model key the C++ build reads; 托盘.ico is embedded by the updater.
NAMES = {'requirements.txt', '托盘.ico'}

def digest(data):
    # Line endings differ between checkouts; only content counts.
    return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()

def symbols(tree):
    result = []
    def walk(nodes, prefix=''):
        for node in nodes:
            if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                name = prefix + node.name
                result.append({'name': name, 'line': node.lineno, 'end_line': node.end_lineno,
                               'kind': 'class' if isinstance(node, ast.ClassDef) else 'function'})
                if isinstance(node, ast.ClassDef):
                    walk(node.body, name + '.')
    walk(tree.body)
    return result

def tracked(listed):
    return sorted(rel for rel in listed if rel in NAMES or (
        rel.startswith(ROOTS) and Path(rel).suffix in EXTENSIONS
        and not any(p in {'__pycache__', 'target', 'build', '.pytest_cache'} for p in Path(rel).parts)))

def git(*args):
    return subprocess.check_output(['git', '-C', str(REPOSITORY), *args])

def verify():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    recorded = {f['path']: f['sha256'] for f in manifest['files']}
    live = tracked(git('ls-files', '-z').decode('utf-8').split('\0'))
    changed = [rel for rel in live if rel in recorded and digest((REPOSITORY / rel).read_bytes()) != recorded[rel]]
    print(json.dumps({'caught_up_with': manifest['git_head'], 'changed': changed,
                      'added': [rel for rel in live if rel not in recorded],
                      'removed': sorted(set(recorded) - set(live))}, ensure_ascii=False, indent=2))

def sync(ref):
    """Record a commit as caught up with, reading blobs from git so uncommitted edits never count."""
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    inventory = {m['source']: m for m in json.loads(INVENTORY.read_text(encoding='utf-8'))}
    head = git('rev-parse', ref).decode().strip()
    old = {f['path']: f['sha256'] for f in manifest['files']}
    files, changed = [], []
    for rel in tracked(git('ls-tree', '-r', '-z', '--name-only', head).decode('utf-8').split('\0')):
        data = git('cat-file', 'blob', f'{head}:{rel}')
        sha = digest(data)
        files.append({'path': rel, 'sha256': sha, 'bytes': len(data)})
        if old.get(rel) == sha:
            continue
        changed.append(rel)
        if rel.endswith('.py'):
            tree = ast.parse(data.decode('utf-8-sig'), filename=rel)
            entry = inventory.get(rel, {'source': rel, 'status': 'unported',
                                        'role': 'test' if rel.startswith('main/tests/') else 'production'})
            entry.update(sha256=sha, symbols=symbols(tree))
            inventory[rel] = entry
    removed = sorted(set(old) - {f['path'] for f in files})
    for rel in removed:
        inventory.pop(rel, None)
    manifest = {'git_head': head, 'files': files}
    MANIFEST.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    INVENTORY.write_text(json.dumps(sorted(inventory.values(), key=lambda m: m['source']),
                                    ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'head': head, 'changed': changed, 'removed': removed}, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['verify', 'sync'])
    parser.add_argument('--ref', default='HEAD')
    args = parser.parse_args()
    if args.action == 'sync':
        sync(args.ref)
    else:
        verify()
