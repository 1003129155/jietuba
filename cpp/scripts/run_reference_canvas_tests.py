"""Run frozen canvas tests in a temporary copy with real compiled translations.

The freeze excludes .qm build products. Compile its XML there, rather than
changing the baseline or copying binaries from a drifting live application.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import PySide6

ROOT = Path(__file__).resolve().parents[1]
build = (ROOT / 'build').resolve()
build.mkdir(exist_ok=True)
manifest = json.loads((ROOT / 'migration/source_manifest.json').read_text(encoding='utf-8'))
report = ROOT / 'docs/validation/python-arrow-canvas-tests.txt'
with tempfile.TemporaryDirectory(prefix='reference-canvas-', dir=build) as directory:
    stage = Path(directory).resolve()
    assert stage.parent == build
    for entry in manifest['files']:
        destination = (stage / entry['path']).resolve()
        assert destination.is_relative_to(stage)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT.parent / entry['path'], destination)
    tool = Path(PySide6.__file__).parent / 'lrelease.exe'
    if not tool.is_file(): raise RuntimeError('Reference PySide6 lrelease.exe missing.')
    for source in (stage / 'main/translations').glob('app_*.xml'):
        subprocess.run([str(tool), str(source), '-qm', str(source.with_suffix('.qm'))], check=True, capture_output=True)
    environment = dict(os.environ, QT_QPA_PLATFORM='offscreen', PYTHONUTF8='1')
    tests = ['test_arrow_styles.py', 'test_drawing_items.py', 'test_undo_stack.py', 'test_undo_number.py']
    with report.open('w', encoding='utf-8') as output:
        result = subprocess.run([sys.executable, '-m', 'pytest', '-q'] +
            [str(stage / 'main/tests' / name) for name in tests], cwd=stage,
            env=environment, stdout=output, stderr=subprocess.STDOUT)
print(report.read_text(encoding='utf-8'))
sys.exit(result.returncode)
