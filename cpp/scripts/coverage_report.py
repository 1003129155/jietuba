"""Merges the Cobertura reports of a coverage run into one report on cpp/src.

Each test executable is a separate package in the collector's report and the same source line
appears once per executable; a line counts as covered if any of them hit it. Paths are written
relative to the repository root with the case git uses, which is what diff-cover matches against.

    python coverage_report.py raw1.xml [raw2.xml ...] --out coverage.xml --summary summary.txt [--fail-under 60]
        [--changed-since origin/master2]

A source file no test executable links never appears in the collector's report, and diff-cover
skips files without data; --changed-since fails when a changed .cpp under cpp/src has none.
"""
import argparse
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def tracked_sources():
    names = subprocess.run(["git", "ls-files", "cpp/src"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
    return {name.lower(): name for name in names}


def collect(reports):
    tracked = tracked_sources()
    prefix = str(REPO).replace("\\", "/").lower().rstrip("/") + "/"
    files = {}
    for report in reports:
        for cls in ET.parse(report).getroot().iter("class"):
            path = cls.get("filename", "").replace("\\", "/")
            lowered = path.lower()
            if lowered.startswith(prefix):
                lowered = lowered[len(prefix):]
            name = tracked.get(lowered)
            if name is None:
                continue
            lines = files.setdefault(name, {})
            for line in cls.iter("line"):
                number = int(line.get("number"))
                lines[number] = lines.get(number, False) or int(line.get("hits", "0")) > 0
    return files


def write_cobertura(files, out):
    total = sum(len(lines) for lines in files.values())
    covered = sum(sum(lines.values()) for lines in files.values())
    rate = lambda c, t: f"{c / t:.4f}" if t else "1"
    root = ET.Element("coverage", {"line-rate": rate(covered, total), "lines-valid": str(total), "lines-covered": str(covered),
                                   "branch-rate": "0", "version": "1", "timestamp": "0"})
    ET.SubElement(ET.SubElement(root, "sources"), "source").text = "."
    classes = ET.SubElement(ET.SubElement(ET.SubElement(root, "packages"), "package", {"name": "cpp/src", "line-rate": rate(covered, total)}), "classes")
    for name in sorted(files):
        lines = files[name]
        cls = ET.SubElement(classes, "class", {"name": name, "filename": name, "line-rate": rate(sum(lines.values()), len(lines))})
        node = ET.SubElement(cls, "lines")
        for number in sorted(lines):
            ET.SubElement(node, "line", {"number": str(number), "hits": "1" if lines[number] else "0"})
    ET.ElementTree(root).write(out, encoding="utf-8", xml_declaration=True)
    return covered, total


def write_summary(files, covered, total, summary):
    rows = sorted(((len(l) - sum(l.values()), len(l), sum(l.values()), n) for n, l in files.items()), reverse=True)
    width = max((len(n) for *_, n in rows), default=4)
    text = [f"{'File':<{width}}  {'Lines':>6}  {'Covered':>7}  {'Missed':>6}  {'%':>6}"]
    for missed, lines, hit, name in rows:
        text.append(f"{name:<{width}}  {lines:>6}  {hit:>7}  {missed:>6}  {100 * hit / lines if lines else 100:>6.1f}")
    text.append(f"TOTAL {covered} / {total} lines = {100 * covered / max(total, 1):.1f}%")
    Path(summary).write_text("\n".join(text) + "\n", encoding="utf-8")
    return text[-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reports", nargs="+")
    parser.add_argument("--out", required=True)
    parser.add_argument("--summary", required=True)
    parser.add_argument("--fail-under", type=float, default=0)
    parser.add_argument("--changed-since")
    args = parser.parse_args()
    files = collect(args.reports)
    covered, total = write_cobertura(files, args.out)
    print(write_summary(files, covered, total, args.summary))
    failed = False
    percent = 100 * covered / max(total, 1)
    if percent < args.fail_under:
        print(f"Line coverage of cpp/src fell to {percent:.1f}%, below the floor of {args.fail_under}%", file=sys.stderr)
        failed = True
    if args.changed_since:
        changed = subprocess.run(["git", "diff", "--name-only", "--diff-filter=AM", f"{args.changed_since}...HEAD", "--", "cpp/src/*.cpp"],
                                 cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
        for name in changed:
            if name not in files:
                print(f"{name} changed but no test runs any of it", file=sys.stderr)
                failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
