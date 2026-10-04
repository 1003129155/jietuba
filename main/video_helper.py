"""录制扩展的子进程入口与已安装 wheel 的许可来源。"""

from __future__ import annotations

from importlib import metadata, util
from pathlib import Path
import sys


PACKAGE_NAME = "j-video-recorder"
LICENSE_NAMES = ("LICENSE", "THIRD-PARTY-NOTICES.txt")


def worker_command() -> list[str]:
    """GUI 只查找扩展，不导入原生媒体组件；子进程负责实际加载。"""
    if util.find_spec("video_recorder") is None:
        raise FileNotFoundError(f"{PACKAGE_NAME} is not installed")
    if getattr(sys, "frozen", False):
        return [sys.executable, "--video-recorder-worker"]
    worker = Path(__file__).with_name("video_worker.py")
    if not worker.is_file():
        raise FileNotFoundError(worker)
    return [sys.executable, "-u", str(worker)]


def installed_recorder_licenses() -> dict[str, Path]:
    """打包许可来自本次安装的扩展 wheel，避免混用仓库中的旧声明。"""
    try:
        distribution = metadata.distribution(PACKAGE_NAME)
    except metadata.PackageNotFoundError as error:
        raise FileNotFoundError(f"{PACKAGE_NAME} is not installed") from error
    result = {}
    for name in LICENSE_NAMES:
        entries = [entry for entry in distribution.files or ()
                   if entry.name == name and any(part.endswith(".dist-info") for part in entry.parts)]
        if len(entries) != 1:
            raise ValueError(f"{PACKAGE_NAME} must contain exactly one {name}")
        path = Path(distribution.locate_file(entries[0])).resolve()
        if not path.is_file():
            raise FileNotFoundError(path)
        result[name] = path
    return result
