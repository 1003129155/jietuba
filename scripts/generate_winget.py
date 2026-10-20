"""Generate WinGet manifests from the final (optionally signed) release ZIPs."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile


REPOSITORY = "https://github.com/1003129155/jietuba"
SCHEMA = "1.6.0"
PACKAGES = (
    ("1003129155.Jietuba", "jietuba_pp", "Jietuba", "jietuba", True),
    ("1003129155.Jietuba.Lite", "jietuba_lite", "Jietuba Lite", "jietuba-lite", False),
)


def scalar(value):
    # JSON strings are also valid YAML quoted scalars.
    return json.dumps(value, ensure_ascii=False)


def document(package, version, kind, fields):
    header = (
        f"# yaml-language-server: $schema=https://aka.ms/winget-manifest.{kind}.{SCHEMA}.schema.json\n"
        f"PackageIdentifier: {scalar(package)}\n"
        f"PackageVersion: {scalar(version)}\n"
    )
    return header + fields + f"ManifestType: {kind}\nManifestVersion: {SCHEMA}\n"


def generate(assets, output, version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Expected a stable version such as 2.1.8")
    tag = f"release-{version}"
    documents = []
    # Validate all inputs before writing any manifests.
    for package, stem, name, alias, full in PACKAGES:
        installers = "InstallerType: zip\nNestedInstallerType: portable\n"
        installers += f"NestedInstallerFiles:\n- RelativeFilePath: {stem}.exe\n  PortableCommandAlias: {alias}\n"
        installers += "Installers:\n"
        for architecture in ("x64", "arm64"):
            archive = assets / f"{stem}-{tag}-{architecture}.zip"
            with zipfile.ZipFile(archive) as bundle:
                members = bundle.namelist()
                if members.count(f"{stem}.exe") != 1:
                    raise ValueError(f"{archive}: expected exactly one root-level {stem}.exe")
                if full and not any(p.startswith("models/") and not p.endswith("/") for p in members):
                    raise ValueError(f"{archive}: full edition is missing model files")
                corrupt = bundle.testzip()
                if corrupt:
                    raise ValueError(f"{archive}: corrupt member {corrupt}")
            with archive.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest().upper()
            installers += (
                f"- Architecture: {architecture}\n"
                f"  InstallerUrl: {scalar(f'{REPOSITORY}/releases/download/{tag}/{archive.name}')}\n"
                f"  InstallerSha256: {digest}\n"
            )
        locale = {
            "PackageLocale": "en-US",
            "Publisher": "1003129155",
            "PublisherUrl": "https://github.com/1003129155",
            "PublisherSupportUrl": f"{REPOSITORY}/issues",
            "PackageName": name,
            "PackageUrl": REPOSITORY,
            "License": "MIT",
            "LicenseUrl": f"{REPOSITORY}/blob/{tag}/LICENSE",
            "ShortDescription": "Windows screenshot, OCR, annotation and clipboard tool" + (" (lite edition)." if not full else "."),
            "ReleaseNotesUrl": f"{REPOSITORY}/releases/tag/{tag}",
        }
        folder = output / "manifests" / "1" / Path(*package.split(".")) / version
        documents.extend([
            (folder / f"{package}.yaml", document(package, version, "version", "DefaultLocale: en-US\n")),
            (folder / f"{package}.installer.yaml", document(package, version, "installer", installers)),
            (folder / f"{package}.locale.en-US.yaml", document(package, version, "defaultLocale", "".join(f"{key}: {scalar(value)}\n" for key, value in locale.items()))),
        ])
    for path, content in documents:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
    return [str(path) for path, _ in documents]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True, help="Directory containing all four final release ZIPs")
    parser.add_argument("--output", type=Path, default=Path("build/winget"))
    parser.add_argument("--version", default=Path(__file__).resolve().parents[1].joinpath("VERSION").read_text(encoding="utf-8").strip())
    parser.add_argument("--archive", type=Path, help="Also package generated manifests as a ZIP at this path")
    args = parser.parse_args()
    for path in generate(args.assets, args.output, args.version):
        print(path)
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        if args.archive.suffix != ".zip":
            parser.error("--archive must end in .zip")
        shutil.make_archive(str(args.archive.with_suffix("")), "zip", root_dir=args.output, base_dir="manifests")


if __name__ == "__main__":
    main()
