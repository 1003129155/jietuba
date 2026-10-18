"""Compiles the interface translations: resources/translations/app_*.xml -> app_*.qm.

The .qm files are committed and embedded by CMake; run this after editing the XML:
    python scripts\\compile_translations.py [path\\to\\lrelease.exe]
Use a Qt 6 lrelease, such as pyside6-lrelease (pip install pyside6-essentials): other versions
write different bytes for the same XML.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

TRANSLATIONS = Path(__file__).resolve().parents[1] / 'resources' / 'translations'


def find_lrelease() -> str:
    if len(sys.argv) > 1:
        return sys.argv[1]
    scripts = Path(sys.executable).parent
    for candidate in (scripts / 'pyside6-lrelease.exe', shutil.which('pyside6-lrelease'), shutil.which('lrelease')):
        if candidate and Path(candidate).exists():
            return str(candidate)
    raise SystemExit('lrelease not found; pass its path or install pyside6-essentials.')


def main() -> None:
    lrelease = find_lrelease()
    for xml in sorted(TRANSLATIONS.glob('app_*.xml')):
        target = xml.with_suffix('.qm')
        subprocess.run([lrelease, '-silent', str(xml), '-qm', str(target)], check=True)
        print(f'{xml.name} -> {target.name}')


if __name__ == '__main__':
    main()
