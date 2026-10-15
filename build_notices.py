"""Combine the applicable license notices into one file beside the executable.

python build_notices.py --bundled build/jietuba_pp-bundled.json
    按上次打包记下的文件清单写出完整版的声明。

python build_notices.py --bundled build/jietuba_pp-bundled.json --bundled build/jietuba_lite-bundled.json \\
        --qt-sbom <Qt SDK 的 sbom 目录>... --qt-licenses <Qt 源码的 LICENSES 目录>...
    重新生成 licenses/pyside6-qt-NOTICES.txt。SBOM 取自同版本官方 Qt SDK 的 sbom 目录
    （qtbase、qtsvg 在主包里，qtimageformats 在 addons 包里），PySide6 带的 Qt 就是这一份构建。
"""

import importlib.metadata
import os
import re
import sys
from collections import deque
from pathlib import Path, PurePosixPath

from main.video.helper import installed_recorder_licenses


REPO_DIR = Path(__file__).resolve().parent
CRATES = ("gifrecorder", "longstitch", "pyclipboard", "ppocr_rust", "hdrcapture", "inputhub", "updater")
QT_NOTICES = REPO_DIR / "licenses" / "pyside6-qt-NOTICES.txt"
# 声明由上面固定的来源提供：自制 Rust 扩展用仓库里的声明，oneocr 的 wheel 不带许可文件
COVERED_DISTRIBUTIONS = {"j-gif", "j-stitch", "j-clipboard", "j-ppocr", "j-hdrcapture", "j-input", "j-video", "oneocr"}
# wheel 自带的许可文件没覆盖编进扩展模块的其他代码；对应的版本变了要一起更新
SUPPLEMENTS = {"zxing-cpp": ("3.1.1", REPO_DIR / "licenses" / "zxing-cpp-bundled-NOTICES.txt")}
_LICENSE_NAME = re.compile(r"(?i)^(licen[cs]e|copying|notice|third[-_]party)")
_QT_BINARY = re.compile(r"^PySide6/(Qt6\w+\.dll|plugins/.+\.dll)$")
_RULE = "=" * 80


def bundled_list_path(exe_name: str) -> Path:
    """build_with_ocr_onefile.py 在 spec 里写下的打包文件清单。"""
    return REPO_DIR / "build" / f"{exe_name}-bundled.json"


def _normalize(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def _is_within(path: str, directory: str) -> bool:
    directory = os.path.normcase(os.path.abspath(directory))
    return path == directory or path.startswith(directory + os.sep)


def _license_files(dist, roots) -> list[Path]:
    files = [f for f in dist.files or () if f.suffix not in (".py", ".pyc")]
    found = [f for f in files if f.parts[0].endswith(".dist-info")
             and (f.parts[1:2] == ("licenses",) or _LICENSE_NAME.match(f.name))]
    # 少数包的许可文件放在各个包目录里，只取打进包的那些目录的
    found = found or [f for f in files if f.parts[0] in roots and _LICENSE_NAME.match(f.name)]
    if not found:
        raise ValueError(f"{dist.metadata['Name']} {dist.version} 没有许可文件，需要在 build_notices.py 里补上来源")
    return [Path(dist.locate_file(f)) for f in found]


def _bundled_sources(bundled) -> list[tuple[str, str]]:
    """打进包的第三方分发包、Python 运行时和 PySide6 带的 Qt 各自的声明。"""
    if not bundled:
        return []
    owners, roots = {}, {}
    for dist in importlib.metadata.distributions():
        for file in dist.files or ():
            owners[os.path.normcase(os.path.abspath(dist.locate_file(file)))] = dist
            if len(file.parts) > 1 and file.parts[0] != ".." and not file.parts[0].endswith((".dist-info", ".data")):
                roots.setdefault(os.path.normcase(os.path.abspath(dist.locate_file(file.parts[0]))), set()).add(dist)
    dists, dist_roots, qt_files, python_runtime = {}, {}, set(), False
    for source in bundled:
        path = os.path.normcase(os.path.abspath(source))
        dist = owners.get(path)
        if dist is None:
            # 运行时生成进包目录的模块（comtypes/gen）不在安装记录里，归到该包目录所属的分发包
            owning = [d for root, ds in roots.items() if _is_within(path, root) for d in ds]
            dist = owning[0] if len({d.metadata["Name"] for d in owning}) == 1 else None
        if dist is not None:
            name = _normalize(dist.metadata["Name"])
            relative = PurePosixPath(Path(source).resolve().relative_to(Path(dist.locate_file("")).resolve()))
            if name == "pyside6-essentials" and _QT_BINARY.match(relative.as_posix()):
                qt_files.add(relative.as_posix())
            if name not in COVERED_DISTRIBUTIONS:
                dists[name] = dist
                dist_roots.setdefault(name, set()).add(relative.parts[0])
        elif "site-packages" in Path(path).parts:
            raise ValueError(f"打包文件不属于任何已安装的分发包: {source}")
        elif _is_within(path, sys.base_prefix):
            python_runtime = True
        elif not (_is_within(path, str(REPO_DIR)) and not _is_within(path, sys.prefix)):
            raise ValueError(f"无法确定打包文件的来源: {source}")
    # 引导程序随每个 exe 分发，不一定出现在清单里
    dists.setdefault("pyinstaller", importlib.metadata.distribution("pyinstaller"))

    sources = []
    if python_runtime:
        license_file = Path(sys.base_prefix) / "LICENSE.txt"
        if not license_file.is_file():
            raise FileNotFoundError(license_file)
        sources.append((f"Python {sys.version.split()[0]}", license_file.read_text(encoding="utf-8")))
    for name in sorted(dists):
        dist = dists[name]
        meta = dist.metadata
        license_line = (meta.get("License-Expression") or meta.get("License") or "").strip().splitlines()
        parts = [f"License: {license_line[0]}"] if license_line else []
        for path in _license_files(dist, dist_roots.get(name, ())):
            parts.append(f"--- {path.name}\n\n{path.read_text(encoding='utf-8', errors='replace').strip()}")
        if name in SUPPLEMENTS:
            version, path = SUPPLEMENTS[name]
            if dist.version != version:
                raise ValueError(f"{path.name} 写的是 {name} {version}，打包的是 {dist.version}，要一起更新")
            parts.append(f"--- {path.name}\n\n{path.read_text(encoding='utf-8').strip()}")
        sources.append((f"{meta['Name']} {dist.version} (PyPI)", "\n\n".join(parts)))
    if qt_files:
        text = QT_NOTICES.read_text(encoding="utf-8")
        version = importlib.metadata.version("pyside6-essentials")
        if f"Qt {version}," not in text.split("Files:", 1)[0]:
            raise ValueError(f"{QT_NOTICES.name} 不是按 Qt {version} 生成的，按 build_notices.py 的说明重新生成")
        covered = set(_covered_qt_files(text))
        missing = sorted(qt_files - covered)
        if missing:
            raise ValueError(f"{QT_NOTICES.name} 没有覆盖 {', '.join(missing)}，按 build_notices.py 的说明重新生成")
        sources.append(("Qt in PySide6", text))
    return sources


def write_notices(output_dir: Path, *, lite: bool = False, bundled=()) -> Path:
    sources = [("jietuba", REPO_DIR / "LICENSE")]
    sources.extend(
        (crate, REPO_DIR / "rust_libs" / crate / "THIRD-PARTY-NOTICES.txt")
        for crate in CRATES if not (lite and crate == "ppocr_rust")
    )
    if not lite:
        sources.append(("onnxruntime", REPO_DIR / "rust_libs" / "ppocr_rust" / "ONNXRUNTIME-NOTICES.txt"))
    sources.extend([
        ("windows-capture", REPO_DIR / "rust_libs" / "hdrcapture" / "LICENSE-UPSTREAM"),
        ("oneocr", REPO_DIR / "licenses" / "oneocr-LICENSE.txt"),
        ("lucide icons", REPO_DIR / "licenses" / "lucide-LICENSE.txt"),
        ("tabler icons", REPO_DIR / "licenses" / "tabler-icons-LICENSE.txt"),
    ])
    # 录制扩展的声明来自已安装 wheel，与打包的原生模块保持一致。
    sources.extend((f"video_recorder/{name}", path) for name, path in installed_recorder_licenses().items())
    sections = ["JIETUBA LICENSE AND THIRD-PARTY NOTICES\n"]
    for name, source in sources:
        sections.append(f"{_RULE}\n{name}\n{_RULE}\n\n{source.read_text(encoding='utf-8').strip()}\n")
    for name, text in _bundled_sources(bundled):
        sections.append(f"{_RULE}\n{name}\n{_RULE}\n\n{text.strip()}\n")
    output_dir.mkdir(parents=True, exist_ok=True)
    target = output_dir / "THIRD-PARTY-NOTICES.txt"
    target.write_text("\n".join(sections), encoding="utf-8")

    # Remove only the known notices from previous builds; preserve unrelated user files.
    old_dir = output_dir / "licenses"
    if old_dir.is_dir():
        names = ["LICENSE.txt", "windows-capture-LICENSE.txt", "oneocr-LICENSE.txt"]
        names.extend(f"{crate}-THIRD-PARTY-NOTICES.txt" for crate in CRATES)
        names.extend(("video_recorder-LICENSE", "video_recorder-THIRD-PARTY-NOTICES.txt"))
        for name in names:
            (old_dir / name).unlink(missing_ok=True)
        if not any(old_dir.iterdir()):
            old_dir.rmdir()
    return target


def _covered_qt_files(text: str) -> list[str]:
    lines = text.split("Files:\n", 1)[1].split("\n\n", 1)[0]
    return [line.strip() for line in lines.splitlines()]


def _parse_sbom(path: Path, packages: dict, file_ids: dict, owner: dict, deps: dict) -> None:
    """读 tag-value 格式的 SPDX：包、文件、CONTAINS 与 DEPENDS_ON 关系。"""
    lines = path.read_text(encoding="utf-8").split("\n")
    record, i = None, 0
    while i < len(lines):
        match = re.match(r"^(\w+): (.*)$", lines[i])
        if match:
            key, value = match.groups()
            if value.startswith("<text>"):
                buffer = [value[6:]]
                while "</text>" not in buffer[-1]:
                    i += 1
                    buffer.append(lines[i])
                value = "\n".join(buffer)
                value = value[:value.index("</text>")]
            if key in ("PackageName", "FileName", "LicenseID"):
                record = {"kind": key, "name": value}
            elif key == "Relationship":
                # 其他文档的元素写作 DocumentRef-<文档>:<id>，各 SBOM 的 id 互不重复
                source, kind, target = (re.sub(r"^DocumentRef-[^:]+:", "", part) for part in value.split())
                if kind == "CONTAINS":
                    owner[target] = source
                elif kind == "DEPENDS_ON":
                    deps.setdefault(source, []).append(target)
            elif key == "SPDXID" and record is not None:
                if record["kind"] == "PackageName":
                    packages[value] = record
                elif record["kind"] == "FileName":
                    file_ids[record["name"]] = value
            elif record is not None:
                record.setdefault(key, value)
        i += 1


def write_qt_notices(sbom_dirs, license_dirs, qt_files, target: Path = QT_NOTICES) -> Path:
    """按 Qt 的 SBOM 写出 PySide6 带进包的 Qt 文件所含第三方组件的声明，许可证全文附在末尾。"""
    packages, file_ids, owner, deps = {}, {}, {}, {}
    for directory in sbom_dirs:
        for sbom in sorted(Path(directory).glob("*.spdx")):
            if not sbom.name.endswith(".source.spdx"):
                _parse_sbom(sbom, packages, file_ids, owner, deps)
    shipped = []
    for file in sorted(qt_files):
        relative = file.split("/", 1)[1]
        sbom_name = f"./{relative}" if relative.startswith("plugins/") else f"./bin/{relative}"
        if sbom_name not in file_ids:
            raise ValueError(f"{file} 不在 Qt 的 SBOM 里")
        shipped.append(owner[file_ids[sbom_name]])
    seen, queue = set(), deque(shipped)
    while queue:
        package = queue.popleft()
        if package not in seen:
            seen.add(package)
            queue.extend(deps.get(package, ()))

    def display_name(record):
        match = re.search(r"(?m)^\s*Name: (.+)$", record.get("PackageComment", ""))
        return match.group(1).strip() if match else record["name"]

    qt = sorted({packages[p]["name"]: packages[p] for p in shipped}.values(), key=lambda r: r["name"])
    components = {}
    third_party = (packages[p] for p in seen if re.search(r"-qt-(bundled-3rdparty-module|3rdparty-sources)-", p))
    for record in sorted(third_party, key=lambda r: (display_name(r).lower(), r["name"])):
        # Qt 对同一个库的不同包装（ZlibPrivate 与 BundledZLIB）声明相同，只列一次
        components.setdefault((record["PackageLicenseConcluded"], record.get("PackageCopyrightText", "")), record)

    license_ids = {"LGPL-3.0-only"}
    out = [
        "QT IN PYSIDE6: THIRD-PARTY NOTICES",
        "",
        f"Generated by build_notices.py from the SBOMs of Qt {qt[0]['PackageVersion']}, the build the PySide6",
        "wheels ship. PySide6, Shiboken and Qt are used under LGPL-3.0-only.",
        "",
        "Files:",
        *(f"  {file}" for file in sorted(qt_files)),
        "",
        "Qt modules and plugins:",
        *(f"  {record['name']}" for record in qt),
    ]
    for record in components.values():
        expression = record["PackageLicenseConcluded"]
        license_ids.update(t for t in re.findall(r"[A-Za-z0-9.+-]+", expression) if t not in ("AND", "OR", "WITH"))
        version = record.get("PackageVersion", "")
        version = f" {version}" if version and version != "unknown" else ""
        out += ["", "-" * 80, f"{display_name(record)}{version}  [Qt: {record['name']}]",
                f"License: {expression}", "", record.get("PackageCopyrightText", "").strip()]
    out += ["", "-" * 80, "License texts", "-" * 80]
    for license_id in sorted(license_ids, key=str.lower):
        text = next((Path(d) / f"{license_id}.txt" for d in license_dirs if (Path(d) / f"{license_id}.txt").is_file()), None)
        if text is None:
            raise FileNotFoundError(f"没有 {license_id} 的许可证全文")
        out += ["", f"--- {license_id}", "", text.read_text(encoding="utf-8").strip()]
    target.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    return target


if __name__ == "__main__":
    import argparse
    import json

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lite", action="store_true")
    parser.add_argument("--bundled", type=Path, action="append", default=[], help="打包时记下的文件清单")
    parser.add_argument("--qt-sbom", type=Path, nargs="+")
    parser.add_argument("--qt-licenses", type=Path, nargs="+")
    args = parser.parse_args()
    bundled = sorted({p for path in args.bundled for p in json.loads(path.read_text(encoding="utf-8"))})
    if args.qt_sbom:
        site = Path(importlib.metadata.distribution("pyside6-essentials").locate_file("")).resolve()
        qt_files = set()
        for source in bundled:
            path = Path(source).resolve()
            if path.is_relative_to(site) and _QT_BINARY.match(path.relative_to(site).as_posix()):
                qt_files.add(path.relative_to(site).as_posix())
        print(write_qt_notices(args.qt_sbom, args.qt_licenses or [], qt_files))
    else:
        print(write_notices(REPO_DIR / ("dist_lite" if args.lite else "dist"), lite=args.lite, bundled=bundled))
