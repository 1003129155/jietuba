"""Developer-only: derives the log and interface translations from the Python version.

Run with the reference interpreter (it needs PySide6's lrelease):
    ..\\venv311\\Scripts\\python.exe scripts\\generate_resources.py
"""
from __future__ import annotations

import importlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE_MAIN = ROOT.parent / 'main'
RESOURCES = ROOT / 'resources'


def log_translations():
    sys.path.insert(0, str(REFERENCE_MAIN))
    try:
        module = importlib.import_module('core.log_translations')
    finally:
        sys.path.pop(0)
    target = RESOURCES / 'core' / 'log_translations.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(module.TRANSLATIONS, ensure_ascii=False, indent=0, sort_keys=True) + '\n',
                      encoding='utf-8')
    print(f'{len(module.TRANSLATIONS)} log translations -> {target.relative_to(ROOT)}')


def find_lrelease():
    scripts = Path(sys.executable).parent
    for candidate in (scripts / 'pyside6-lrelease.exe', shutil.which('pyside6-lrelease'), shutil.which('lrelease')):
        if candidate and Path(candidate).exists():
            return str(candidate)
    raise SystemExit('lrelease not found; run with the reference venv interpreter.')


def ui_translations():
    lrelease = find_lrelease()
    target_dir = RESOURCES / 'translations'
    target_dir.mkdir(parents=True, exist_ok=True)
    for xml in sorted((REFERENCE_MAIN / 'translations').glob('app_*.xml')):
        target = target_dir / (xml.stem + '.qm')
        subprocess.run([lrelease, '-silent', str(xml), '-qm', str(target)], check=True)
        print(f'{xml.name} -> {target.relative_to(ROOT)}')


if __name__ == '__main__':
    log_translations()
    ui_translations()
