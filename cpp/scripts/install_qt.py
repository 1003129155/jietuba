"""Downloads the Qt MSVC x64 SDK (qtbase, qtsvg, qtimageformats) into .sdk/<version>/msvc2022_64.

From Qt 6.11 the online repository has one folder per compiler and the archives no
longer contain the <version>/msvc2022_64 prefix; only that layout is handled here.
On an existing SDK only the modules it lacks (no SBOM of theirs) are added.
    .\\.tools\\Scripts\\python.exe scripts\\install_qt.py [--version 6.11.0]
"""
import argparse
import hashlib
import io
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

import py7zr

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
# (package, archive): qtbase and qtsvg come in the main package, qtimageformats (WebP, TIFF) in an add-on.
MODULES = (("", "qtbase"), ("", "qtsvg"), ("addons.qtimageformats.", "qtimageformats"))


def fetch(url):
    with urllib.request.urlopen(url, timeout=300) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default="6.11.0")
    parser.add_argument("--sdk-dir", type=Path, default=Path(__file__).resolve().parents[1] / ".sdk")
    args = parser.parse_args()
    tag = args.version.replace(".", "")
    target = args.sdk_dir / args.version / "msvc2022_64"
    fresh = not target.exists()
    destination = target.with_name(target.name + ".partial") if fresh else target
    if fresh:
        destination.mkdir(parents=True, exist_ok=False)
    repo = f"{BASE}/qt6_{tag}/qt6_{tag}_msvc2022_64"
    updates = ET.fromstring(fetch(f"{repo}/Updates.xml"))
    for addon, module in MODULES:
        if (target / "sbom" / f"{module}-{args.version}.spdx").exists():
            continue
        package = f"qt.qt6.{tag}.{addon}win64_msvc2022_64"
        node = next(p for p in updates.iter("PackageUpdate") if p.findtext("Name") == package)
        archives = [a.strip() for a in node.findtext("DownloadableArchives").split(",")]
        name = next(a for a in archives if a.startswith(module + "-"))
        url = f"{repo}/{package}/{node.findtext('Version')}{name}"
        data = fetch(url)
        # The checksum comes from download.qt.io itself; the archive may come from a mirror.
        expected = fetch(url + ".sha256").split()[0].decode()
        if hashlib.sha256(data).hexdigest() != expected:
            sys.exit(f"checksum mismatch: {name}")
        with py7zr.SevenZipFile(io.BytesIO(data)) as archive:
            for member in archive.getnames():
                path = PurePosixPath(member.replace("\\", "/"))
                if path.is_absolute() or ".." in path.parts:
                    sys.exit(f"unsafe path in {name}: {member}")
            archive.extractall(destination)
        print(f"{module}: {len(data) / 2**20:.1f} MB")
    if fresh:
        # Lets qmake, qtpaths and windeployqt resolve the SDK relative to bin.
        (destination / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="ascii")
        destination.rename(target)
    print(target)


if __name__ == "__main__":
    main()
