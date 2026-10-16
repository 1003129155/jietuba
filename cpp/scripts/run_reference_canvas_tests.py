"""Run the Python version's canvas tests in a temporary copy with real compiled translations.

.qm files are build products and not tracked; the copy compiles them from the XML, so the
Python source tree is left untouched.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import PySide6

ROOT = Path(__file__).resolve().parents[1]
build = (ROOT / 'build').resolve()
build.mkdir(exist_ok=True)
report = build / 'python-canvas-tests.txt'
tracked = subprocess.check_output(['git', '-C', str(ROOT.parent), 'ls-files', '-z', 'main', 'svg']).decode('utf-8').split('\0')
with tempfile.TemporaryDirectory(prefix='reference-canvas-', dir=build) as directory:
    stage = Path(directory).resolve()
    assert stage.parent == build
    for rel in filter(None, tracked):
        destination = (stage / rel).resolve()
        assert destination.is_relative_to(stage)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT.parent / rel, destination)
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
