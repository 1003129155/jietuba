import importlib.util
from pathlib import Path


def test_onefile_build_uses_repository_root():
    repository = Path(__file__).parents[2]
    script_path = repository / "build_with_ocr_onefile.py"
    spec = importlib.util.spec_from_file_location("jietuba_build_script", script_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    assert module.SCRIPT_DIR == repository
    assert module.REPO_DIR == repository
    assert (module.REPO_DIR / module.MAIN_APP).is_file()


def test_onefile_build_rejects_foreign_poppler_icu_runtime():
    repository = Path(__file__).parents[2]
    source = (repository / "build_with_ocr_onefile.py").read_text(encoding="utf-8")

    assert "'icuuc.dll'" in source
    assert "'icudt'" in source
