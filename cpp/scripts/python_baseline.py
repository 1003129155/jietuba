"""Records the Python commit the C++ implementation was last checked against. Never imported by the EXE.

    python_baseline.py verify             lists the Python files changed since that commit
    python_baseline.py sync [--ref REF]   records REF (default HEAD) once C++ matches it
"""
from __future__ import annotations
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
BASELINE = ROOT / 'parity' / 'python_baseline.json'
# .json brings in svg/emoji_groups.json, the group icon picker's data.
# .lock: the updater is built with the pinned versions of rust_libs.
EXTENSIONS = {'.py', '.rs', '.toml', '.lock', '.svg', '.qss', '.xml', '.qrc', '.ico', '.txt', '.yml', '.yaml', '.bat', '.json'}
# Behaviour lives in the sources; tests and documents change with it and would only add noise.
ROOTS = ('main/', 'rust_libs/', 'svg/', 'licenses/')
# requirements.txt pins oneocr, whose model key the C++ build reads; 托盘.ico is the application icon.
NAMES = {'requirements.txt', '托盘.ico'}


def digest(data):
    # Line endings differ between checkouts; only content counts.
    return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()


def tracked(listed):
    return sorted(rel for rel in listed if rel in NAMES or (
        rel.startswith(ROOTS) and not rel.startswith('main/tests/') and Path(rel).suffix in EXTENSIONS
        and not any(p in {'__pycache__', 'target', 'build', '.pytest_cache'} for p in Path(rel).parts)))


def git(*args):
    return subprocess.check_output(['git', '-C', str(REPOSITORY), *args])


def verify():
    baseline = json.loads(BASELINE.read_text(encoding='utf-8'))
    recorded = {f['path']: f['sha256'] for f in baseline['files']}
    live = tracked(git('ls-files', '-z').decode('utf-8').split('\0'))
    changed = [rel for rel in live if rel in recorded and digest((REPOSITORY / rel).read_bytes()) != recorded[rel]]
    print(json.dumps({'checked_against': baseline['git_head'], 'changed': changed,
                      'added': [rel for rel in live if rel not in recorded],
                      'removed': sorted(set(recorded) - set(live))}, ensure_ascii=False, indent=2))


def sync(ref):
    """Reads blobs from git, so uncommitted edits never count."""
    old = {f['path']: f['sha256'] for f in json.loads(BASELINE.read_text(encoding='utf-8'))['files']}
    head = git('rev-parse', ref).decode().strip()
    files = []
    for rel in tracked(git('ls-tree', '-r', '-z', '--name-only', head).decode('utf-8').split('\0')):
        data = git('cat-file', 'blob', f'{head}:{rel}')
        files.append({'path': rel, 'sha256': digest(data), 'bytes': len(data)})
    BASELINE.write_text(json.dumps({'git_head': head, 'files': files}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'head': head, 'changed': [f['path'] for f in files if old.get(f['path']) != f['sha256']],
                      'removed': sorted(set(old) - {f['path'] for f in files})}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['verify', 'sync'])
    parser.add_argument('--ref', default='HEAD')
    args = parser.parse_args()
    if args.action == 'sync':
        sync(args.ref)
    else:
        verify()
