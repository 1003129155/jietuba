"""扩展子进程入口和许可来源，不加载实际录制扩展。"""

from importlib import metadata
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

from main import video_helper


def test_source_worker_uses_current_python_without_importing_native(monkeypatch):
    requested = []
    monkeypatch.setattr(video_helper.sys, "frozen", False, raising=False)
    monkeypatch.setattr(video_helper.util, "find_spec", lambda name: requested.append(name) or object())
    command = video_helper.worker_command()
    assert command[:2] == [video_helper.sys.executable, "-u"]
    assert Path(command[2]) == Path(video_helper.__file__).with_name("video_worker.py")
    assert requested == ["video_recorder"]


def test_frozen_worker_reuses_application_entrypoint(monkeypatch):
    monkeypatch.setattr(video_helper.sys, "frozen", True, raising=False)
    monkeypatch.setattr(video_helper.util, "find_spec", lambda name: object())
    assert video_helper.worker_command() == [video_helper.sys.executable, "--video-recorder-worker"]


def test_missing_native_extension_fails_without_searching_path(monkeypatch, tmp_path):
    (tmp_path / "jietuba_video_recorder.exe").write_bytes(b"obsolete helper")
    monkeypatch.setenv("PATH", str(tmp_path))
    monkeypatch.setattr(video_helper.util, "find_spec", lambda name: None)
    with pytest.raises(FileNotFoundError, match="j-video"):
        video_helper.worker_command()


def test_missing_source_worker_reports_missing_file(monkeypatch, tmp_path):
    monkeypatch.setattr(video_helper.sys, "frozen", False, raising=False)
    monkeypatch.setattr(video_helper, "__file__", str(tmp_path / "video_helper.py"))
    monkeypatch.setattr(video_helper.util, "find_spec", lambda name: object())
    with pytest.raises(FileNotFoundError, match="video_worker"):
        video_helper.worker_command()


@pytest.fixture
def installed_wheel(monkeypatch, tmp_path):
    entries = []
    for name in video_helper.LICENSE_NAMES:
        entry = metadata.PackagePath("j_video-0.1.0.dist-info/licenses/" + name)
        path = tmp_path / entry
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("installed wheel " + name, encoding="utf-8")
        entries.append(entry)
    distribution = SimpleNamespace(files=entries, locate_file=lambda entry: tmp_path / entry)
    monkeypatch.setattr(video_helper.metadata, "distribution", lambda name: distribution)
    return distribution


def test_licenses_come_from_installed_distribution(installed_wheel):
    files = video_helper.installed_recorder_licenses()
    assert set(files) == set(video_helper.LICENSE_NAMES)
    for name, path in files.items():
        assert path.read_text(encoding="utf-8") == "installed wheel " + name


def test_missing_distribution_does_not_use_repository_licenses(monkeypatch):
    def missing(name):
        raise metadata.PackageNotFoundError(name)
    monkeypatch.setattr(video_helper.metadata, "distribution", missing)
    with pytest.raises(FileNotFoundError, match="not installed"):
        video_helper.installed_recorder_licenses()


@pytest.mark.parametrize("problem", ["missing_record", "duplicate_record", "missing_file", "empty_record"])
def test_license_records_must_exist_and_be_unambiguous(installed_wheel, problem):
    entry = installed_wheel.files[0]
    if problem == "missing_record":
        installed_wheel.files.remove(entry)
    elif problem == "duplicate_record":
        installed_wheel.files.append(entry)
    elif problem == "empty_record":
        installed_wheel.files = None
    else:
        Path(installed_wheel.locate_file(entry)).unlink()
    with pytest.raises((ValueError, FileNotFoundError)):
        video_helper.installed_recorder_licenses()


@pytest.fixture
def build_script():
    path = Path(__file__).parents[2] / "build_with_ocr_onefile.py"
    spec = importlib.util.spec_from_file_location("video_extension_build_script", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_onefile_collects_licenses_from_installed_production_extension(installed_wheel, build_script, monkeypatch):
    monkeypatch.setitem(sys.modules, "video_recorder", SimpleNamespace(Recorder=type("Recorder", (), {})))
    assert build_script.video_recorder_assets() == video_helper.installed_recorder_licenses()


def test_onefile_rejects_test_support_extension(installed_wheel, build_script, monkeypatch):
    test_recorder = type("Recorder", (), {"_configure_test": lambda self: None})
    monkeypatch.setitem(sys.modules, "video_recorder", SimpleNamespace(Recorder=test_recorder))
    with pytest.raises(RuntimeError, match="test-support"):
        build_script.video_recorder_assets()


@pytest.mark.parametrize("lite", [False, True])
def test_release_notices_include_installed_video_wheel_and_keep_user_files(
    installed_wheel, monkeypatch, tmp_path, lite,
):
    import build_notices

    repository = tmp_path / "repository"
    monkeypatch.setattr(build_notices, "REPO_DIR", repository)
    sources = [repository / "LICENSE", repository / "licenses" / "oneocr-LICENSE.txt",
               repository / "licenses" / "lucide-LICENSE.txt",
               repository / "rust_libs" / "hdrcapture" / "LICENSE-UPSTREAM"]
    sources.extend(repository / "rust_libs" / crate / "THIRD-PARTY-NOTICES.txt"
                   for crate in build_notices.CRATES)
    for source in sources:
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(source.relative_to(repository).as_posix(), encoding="utf-8")

    output = tmp_path / "发行目录"
    old_licenses = output / "licenses"
    old_licenses.mkdir(parents=True)
    for name in video_helper.LICENSE_NAMES:
        (old_licenses / f"video_recorder-{name}").write_text("old notice", encoding="utf-8")
    user_file = old_licenses / "用户文件.txt"
    user_file.write_text("keep", encoding="utf-8")

    target = build_notices.write_notices(output, lite=lite)
    text = target.read_text(encoding="utf-8")
    for name in video_helper.LICENSE_NAMES:
        assert f"installed wheel {name}" in text
        assert not (old_licenses / f"video_recorder-{name}").exists()
    assert "rust_libs/updater/THIRD-PARTY-NOTICES.txt" in text
    assert "licenses/oneocr-LICENSE.txt" in text
    assert "licenses/lucide-LICENSE.txt" in text
    assert ("rust_libs/ppocr_rust/THIRD-PARTY-NOTICES.txt" in text) is not lite
    assert user_file.read_text(encoding="utf-8") == "keep"
