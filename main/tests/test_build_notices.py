"""build_notices.py：按打包清单附上第三方分发包、Python 运行时和 PySide6 所带 Qt 的声明。"""
import importlib.metadata
import os
import re
import sys
from pathlib import Path

import pytest

import build_notices


def _sections(text):
    return re.findall(r"(?m)^={80}\n(.+)\n={80}$", text)


def _write(tmp_path, bundled):
    return build_notices.write_notices(tmp_path / "out", lite=True, bundled=bundled).read_text(encoding="utf-8")


def test_bundled_files_bring_in_their_distributions_python_and_bootloader(tmp_path):
    import mss
    import PIL
    import zxingcpp

    text = _write(tmp_path, [PIL.__file__, mss.__file__, os.__file__, zxingcpp.__file__])
    names = _sections(text)
    assert f"pillow {importlib.metadata.version('pillow')} (PyPI)" in names
    assert f"mss {importlib.metadata.version('mss')} (PyPI)" in names
    assert f"Python {sys.version.split()[0]}" in names
    assert f"pyinstaller {importlib.metadata.version('pyinstaller')} (PyPI)" in names
    assert "libzint and libzueci (BSD-3-Clause)" in text


def test_without_bundle_list_no_distribution_sections(tmp_path):
    assert not any(name.endswith("(PyPI)") for name in _sections(_write(tmp_path, [])))


def test_distributions_with_fixed_notices_are_not_repeated(tmp_path):
    import oneocr

    names = _sections(_write(tmp_path, [oneocr.__file__]))
    assert "oneocr" in names
    assert not any(name.startswith("oneocr ") for name in names)


def test_package_directory_license_only_from_bundled_directories(tmp_path):
    import win32api

    text = _write(tmp_path, [win32api.__file__])
    section = text.split("\npywin32 ", 1)[1].split("=" * 80 + "\n")[1]
    assert "--- license.txt" in section
    assert "adodbapi" not in section.lower()


def test_file_of_unknown_origin_is_an_error(tmp_path):
    stray = tmp_path / "stray.dll"
    stray.write_bytes(b"")
    with pytest.raises(ValueError, match="来源"):
        _write(tmp_path, [str(stray)])


def test_qt_binaries_need_coverage_by_the_qt_notices(tmp_path):
    import PySide6

    package = Path(PySide6.__file__).parent
    assert "Qt in PySide6" in _sections(_write(tmp_path, [str(package / "Qt6Core.dll")]))
    with pytest.raises(ValueError, match="qtuiotouchplugin"):
        _write(tmp_path, [str(package / "plugins" / "generic" / "qtuiotouchplugin.dll")])


def test_supplement_of_another_version_is_an_error(tmp_path, monkeypatch):
    import zxingcpp

    _version, path = build_notices.SUPPLEMENTS["zxing-cpp"]
    monkeypatch.setitem(build_notices.SUPPLEMENTS, "zxing-cpp", ("0.0.1", path))
    with pytest.raises(ValueError, match="0.0.1"):
        _write(tmp_path, [zxingcpp.__file__])


SBOM = """\
SPDXVersion: SPDX-2.3
SPDXID: SPDXRef-DOCUMENT

PackageName: Gui
SPDXID: SPDXRef-Package-qtbase-qt-module-Gui
PackageVersion: 6.11.0
PackageLicenseConcluded: LicenseRef-Qt-Commercial OR LGPL-3.0-only

FileName: ./bin/Qt6Gui.dll
SPDXID: SPDXRef-PackagedFile-Gui

PackageName: BundledLibpng
SPDXID: SPDXRef-Package-qtbase-qt-bundled-3rdparty-module-BundledLibpng
PackageVersion: 1.6.55
PackageLicenseConcluded: Libpng
PackageCopyrightText: <text>Copyright (c) The PNG Reference Library Authors
Copyright (c) Cosmin Truta</text>
PackageComment: <text>
CMake target name: BundledLibpng
    Name: LibPNG
</text>

PackageName: ZlibPrivate
SPDXID: SPDXRef-Package-qtbase-qt-bundled-3rdparty-module-ZlibPrivate
PackageVersion: 1.3.2
PackageLicenseConcluded: Zlib
PackageCopyrightText: <text>(C) Jean-loup Gailly and Mark Adler</text>

Relationship: SPDXRef-Package-qtbase-qt-module-Gui CONTAINS SPDXRef-PackagedFile-Gui
Relationship: SPDXRef-Package-qtbase-qt-module-Gui DEPENDS_ON SPDXRef-Package-qtbase-qt-bundled-3rdparty-module-BundledLibpng
Relationship: DocumentRef-qtbase:SPDXRef-Package-qtbase-qt-bundled-3rdparty-module-BundledLibpng DEPENDS_ON SPDXRef-Package-qtbase-qt-bundled-3rdparty-module-ZlibPrivate
"""


def test_qt_notices_follow_shipped_files_through_dependencies(tmp_path):
    sbom_dir, license_dir = tmp_path / "sbom", tmp_path / "LICENSES"
    sbom_dir.mkdir()
    license_dir.mkdir()
    (sbom_dir / "qtbase-6.11.0.spdx").write_text(SBOM, encoding="utf-8")
    for license_id in ("LGPL-3.0-only", "Libpng", "Zlib"):
        (license_dir / f"{license_id}.txt").write_text(f"{license_id} text", encoding="utf-8")

    target = build_notices.write_qt_notices([sbom_dir], [license_dir], {"PySide6/Qt6Gui.dll"}, tmp_path / "qt.txt")
    text = target.read_text(encoding="utf-8")
    assert "Qt 6.11.0," in text
    assert build_notices._covered_qt_files(text) == ["PySide6/Qt6Gui.dll"]
    assert "LibPNG 1.6.55  [Qt: BundledLibpng]" in text
    assert "Copyright (c) Cosmin Truta" in text
    assert "ZlibPrivate 1.3.2  [Qt: ZlibPrivate]" in text
    assert "--- Zlib\n\nZlib text" in text

    (license_dir / "Zlib.txt").unlink()
    with pytest.raises(FileNotFoundError, match="Zlib"):
        build_notices.write_qt_notices([sbom_dir], [license_dir], {"PySide6/Qt6Gui.dll"}, tmp_path / "qt.txt")


def test_committed_qt_notices_cover_the_installed_pyside6():
    text = build_notices.QT_NOTICES.read_text(encoding="utf-8")
    assert f"Qt {importlib.metadata.version('pyside6-essentials')}," in text.split("Files:", 1)[0]
