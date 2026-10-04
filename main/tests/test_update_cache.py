import json
import os
import subprocess
import sys
import time

import pytest
from PySide6.QtCore import QLockFile, QProcess

from core import update_cache
from core.updater_process import UpdaterProcess


@pytest.fixture
def cache(monkeypatch, tmp_path):
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    root = update_cache.cache_root()
    yield root
    lease = update_cache._sessions.pop(root, None)
    if lease:
        lease.unlock()


def directory(root, name, files, old=False):
    path = root / name
    path.mkdir(parents=True)
    for filename, content in files.items():
        (path / filename).write_bytes(content)
    if old:
        timestamp = time.time() - 8 * 86400
        for file in path.iterdir():
            os.utime(file, (timestamp, timestamp))
        os.utime(path, (timestamp, timestamp))
    return path


def test_sweep_removes_old_cache_and_preserves_recent_files_and_backups(cache):
    old_stage = directory(cache, "a" * 32, {"app.exe": b"old"}, old=True)
    old_helper = directory(cache / "helpers", "b" * 32, {"jietuba_updater.exe": b"old"}, old=True)
    requests = directory(cache, "requests", {"c" * 32 + ".json": b"{}", "user.json": b"keep"}, old=True)
    recent_stage = directory(cache, "d" * 32, {"app.exe": b"recent"})
    backup = directory(cache.parent / ".jietuba-update", "e" * 32, {"backup.exe": b"backup"}, old=True)
    update_cache.begin_session()
    assert not old_stage.exists() and not old_helper.exists()
    assert not (requests / ("c" * 32 + ".json")).exists()
    assert (requests / "user.json").read_bytes() == b"keep"
    assert recent_stage.exists() and backup.exists()


def test_live_application_session_prevents_sweep(cache):
    old_stage = directory(cache, "a" * 32, {"app.exe": b"still in use"}, old=True)
    lease = QLockFile(str(cache / ("b" * 32 + ".session.lock")))
    lease.setStaleLockTime(0)
    assert lease.tryLock(0)
    try:
        update_cache.begin_session()
        assert old_stage.exists()
    finally:
        lease.unlock()


def test_crashed_session_lock_does_not_prevent_cleanup(cache):
    old_stage = directory(cache, "a" * 32, {"package.part": b"orphan"}, old=True)
    lock = cache / ("b" * 32 + ".session.lock")
    subprocess.run([sys.executable, "-c", (
        "import os, sys; from PySide6.QtCore import QLockFile; "
        "lease = QLockFile(sys.argv[1]); lease.setStaleLockTime(0); "
        "assert lease.tryLock(0); os._exit(0)"
    ), str(lock)], check=True, timeout=10)
    assert lock.exists()
    update_cache.begin_session()
    assert not lock.exists() and not old_stage.exists()


def test_unfinished_worker_preserves_its_stage(cache):
    stage = directory(cache, "a" * 32, {"app.exe": b"pending"}, old=True)
    worker = directory(cache, "b" * 32, {"request.json": json.dumps({"staged": str(stage / "app.exe")}).encode()}, old=True)
    update_cache.begin_session()
    assert stage.exists() and worker.exists()


def test_finished_worker_cache_can_be_swept(cache):
    worker = directory(cache, "b" * 32, {"request.json": b"{}", "result.json": b"{}", "jietuba_updater.exe": b"old"}, old=True)
    update_cache.begin_session()
    assert not worker.exists()


def test_cleanup_refuses_nested_directories_and_outside_paths(cache, tmp_path):
    nested = directory(cache, "a" * 32, {"app.exe": b"preserve"})
    (nested / "unknown").mkdir()
    outside = directory(tmp_path, "b" * 32, {"app.exe": b"preserve"})
    update_cache.remove_directory(nested)
    update_cache.remove_directory(outside)
    assert (nested / "app.exe").exists() and (outside / "app.exe").exists()


def test_cleanup_refuses_links(cache, tmp_path):
    outside = directory(tmp_path, "b" * 32, {"app.exe": b"preserve"})
    link = cache / ("a" * 32)
    try:
        link.symlink_to(outside, target_is_directory=True)
    except OSError:
        pytest.skip("Directory symlinks unavailable")
    update_cache.remove_directory(link)
    assert (outside / "app.exe").read_bytes() == b"preserve"


@pytest.mark.parametrize("success", [True, False])
def test_process_exit_cleans_helper_and_only_failed_download(cache, qapp, success):
    runner = UpdaterProcess()
    stage = directory(cache, "a" * 32, {"app.exe" if success else "package.part": b"data"})
    helper = directory(cache / "helpers", "b" * 32, {"jietuba_updater.exe": b"helper"})
    runner._helper = helper / "jietuba_updater.exe"
    runner.transaction = stage.name
    runner._command = "download"
    runner._terminal = runner._downloaded = success
    runner._error = not success
    runner._finish(0 if success else 1, QProcess.ExitStatus.NormalExit)
    assert not helper.exists()
    assert stage.exists() is success
    runner.deleteLater()
