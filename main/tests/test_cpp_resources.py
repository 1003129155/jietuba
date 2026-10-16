"""C++ 实现的界面翻译和日志文案由 cpp/scripts/generate_resources.py 从这里生成，核对它们没有落后。"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TRANSLATIONS = sorted((ROOT / "main" / "translations").glob("app_*.xml"))
CPP_RESOURCES = ROOT / "cpp" / "resources"
REGENERATE = "运行 cpp/scripts/generate_resources.py 重新生成 C++ 的资源"


def _lrelease():
    for candidate in (Path(sys.executable).parent / "pyside6-lrelease.exe", shutil.which("pyside6-lrelease")):
        if candidate and Path(candidate).exists():
            return str(candidate)
    pytest.skip("没有 pyside6-lrelease")


@pytest.mark.parametrize("xml", TRANSLATIONS, ids=lambda path: path.stem)
def test_cpp_interface_translations_match(xml, tmp_path):
    compiled = tmp_path / f"{xml.stem}.qm"
    subprocess.run([_lrelease(), "-silent", str(xml), "-qm", str(compiled)], check=True, capture_output=True)
    assert compiled.read_bytes() == (CPP_RESOURCES / "translations" / compiled.name).read_bytes(), REGENERATE


def test_cpp_has_no_translation_without_source():
    assert {p.stem for p in (CPP_RESOURCES / "translations").glob("*.qm")} == {p.stem for p in TRANSLATIONS}, REGENERATE


def test_cpp_log_translations_match():
    from core.log_translations import TRANSLATIONS as LOG_TRANSLATIONS
    expected = json.dumps(LOG_TRANSLATIONS, ensure_ascii=False, indent=0, sort_keys=True) + "\n"
    actual = (CPP_RESOURCES / "core" / "log_translations.json").read_text(encoding="utf-8").replace("\r\n", "\n")
    assert actual == expected, REGENERATE
