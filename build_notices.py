"""Combine the applicable license notices into one file beside the executable."""

from pathlib import Path

from main.video_helper import installed_recorder_licenses


REPO_DIR = Path(__file__).resolve().parent
CRATES = ("gifrecorder", "longstitch", "pyclipboard", "ppocr_rust", "hdrcapture", "inputhub", "updater")


def write_notices(output_dir: Path, *, lite: bool = False) -> Path:
    sources = [("jietuba", REPO_DIR / "LICENSE")]
    sources.extend(
        (crate, REPO_DIR / "rust_libs" / crate / "THIRD-PARTY-NOTICES.txt")
        for crate in CRATES if not (lite and crate == "ppocr_rust")
    )
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
        sections.append(f"{'=' * 80}\n{name}\n{'=' * 80}\n\n{source.read_text(encoding='utf-8').strip()}\n")
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


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lite", action="store_true")
    args = parser.parse_args()
    print(write_notices(REPO_DIR / ("dist_lite" if args.lite else "dist"), lite=args.lite))
