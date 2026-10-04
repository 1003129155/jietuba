"""Best-effort cleanup of updater-owned cache files, never installation backups."""

import json
import os
from pathlib import Path
import re
import stat
import time
import uuid

from PySide6.QtCore import QLockFile


_ID = re.compile(r"[0-9a-f]{32}\Z")
_MAX_AGE = 7 * 24 * 60 * 60
_sessions = {}


def _plain(path, directory=False):
    meta = path.lstat()
    return (not stat.S_ISLNK(meta.st_mode)
            and not getattr(meta, "st_file_attributes", 0) & 0x400
            and (stat.S_ISDIR(meta.st_mode) if directory else stat.S_ISREG(meta.st_mode)))


def cache_root():
    root = Path(os.environ["LOCALAPPDATA"]) / "jietuba" / "updater"
    if root.parent.exists() and not _plain(root.parent, directory=True):
        raise OSError("Updater cache must not be a link or junction")
    root.mkdir(parents=True, exist_ok=True)
    # Refuse cache trees redirected outside the application's directory.
    for path in (root.parent, root):
        if not _plain(path, directory=True):
            raise OSError("Updater cache must not be a link or junction")
    return root


def cache_subdirectory(name):
    if name not in ("helpers", "requests"):
        raise ValueError("Unknown updater cache directory")
    path = begin_session() / name
    path.mkdir(exist_ok=True)
    if not _plain(path, directory=True):
        raise OSError("Updater cache must not be a link or junction")
    return path


def remove_directory(directory):
    """Delete only a flat UUID cache directory; refuse links and subdirectories."""
    try:
        root = cache_root()
        directory = Path(directory)
        if directory.parent not in (root, root / "helpers") or not _ID.fullmatch(directory.name):
            return
        if not _plain(directory.parent, directory=True) or not _plain(directory, directory=True):
            return
        files = list(directory.iterdir())
        if any(not _plain(path) for path in files):
            return
        # A running Windows helper cannot be deleted. Try it before other files.
        files.sort(key=lambda path: path.name != "jietuba_updater.exe")
        for path in files:
            path.unlink()
        directory.rmdir()
    except (OSError, KeyError):
        pass


def remove_request(path):
    try:
        path = Path(path)
        root = cache_root() / "requests"
        if path.parent == root and path.suffix == ".json" and _ID.fullmatch(path.stem):
            if _plain(root, directory=True) and _plain(path):
                path.unlink()
    except (OSError, KeyError):
        pass


def remove_helper(path):
    if path is not None:
        remove_directory(Path(path).parent)


def _sweep(root):
    cutoff = time.time() - _MAX_AGE
    protected = set()
    entries = list(root.iterdir())
    # Incomplete handoffs may still need their staged EXE. Keep those records.
    for directory in entries:
        if _ID.fullmatch(directory.name) and _plain(directory, directory=True):
            request = directory / "request.json"
            if request.exists() and not (directory / "result.json").exists():
                protected.add(directory.name)
                try:
                    if not _plain(request):
                        return
                    staged = Path(json.loads(request.read_text(encoding="utf-8"))["staged"])
                    protected.add(staged.parent.name)
                except (OSError, ValueError, KeyError, TypeError):
                    # An unreadable handoff cannot safely identify its stage.
                    return
    for parent in (root, root / "helpers", root / "requests"):
        if not parent.exists() or not _plain(parent, directory=True):
            continue
        for path in parent.iterdir():
            if parent.name == "requests":
                if _plain(path) and path.stat().st_mtime < cutoff:
                    remove_request(path)
            elif _ID.fullmatch(path.name) and path.name not in protected and _plain(path, directory=True):
                files = list(path.iterdir())
                if all(_plain(file) for file in files) and max(
                        [path.stat().st_mtime, *(file.stat().st_mtime for file in files)]) < cutoff:
                    remove_directory(path)


def begin_session():
    """Sweep old orphans once, with live application sessions excluded by locks."""
    root = cache_root()
    if root in _sessions:
        return root
    lock_path = root / ".maintenance.lock"
    if lock_path.exists() and not _plain(lock_path):
        raise OSError("Updater cache lock must be a regular file")
    maintenance = QLockFile(str(lock_path))
    maintenance.setStaleLockTime(0)
    if not maintenance.tryLock(1000):
        raise OSError("Updater cache is busy; try again")
    try:
        active = False
        for path in root.glob("*.session.lock"):
            if not _ID.fullmatch(path.name.removesuffix(".session.lock")) or not _plain(path):
                active = True
                continue
            probe = QLockFile(str(path))
            probe.setStaleLockTime(0)
            if probe.tryLock(0):
                probe.unlock()
            else:
                active = True
        if not active:
            try:
                _sweep(root)
            except OSError:
                pass
        lease = QLockFile(str(root / (uuid.uuid4().hex + ".session.lock")))
        lease.setStaleLockTime(0)
        if not lease.tryLock(0):
            raise OSError("Cannot reserve updater cache")
        _sessions[root] = lease
    finally:
        maintenance.unlock()
    return root
