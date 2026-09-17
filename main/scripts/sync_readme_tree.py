# -*- coding: utf-8 -*-
"""
把 README 目录树同步到真实代码结构。

    python main/scripts/sync_readme_tree.py          # 预览改动，不写
    python main/scripts/sync_readme_tree.py --write  # 落盘

不重画树，只改行。树里那些手写的东西——`│` 分隔行、`buttons.py / cards.py` 一行
列多个文件、子目录分组、注释对齐——一律原样保留，只动真正需要动的行。整块重新
生成看着简单，但会把这些格式全抹平，而且描述得靠别处存一份，等于把手工维护换个
地方做。

描述只搬运、不发明：
- 文件还在      → 整行不碰，描述天然保住
- 文件没了      → 整行删掉（一行多文件时只摘掉那个 token）
- 文件是新的    → 插一行 `# TODO`，显眼，不会静默留空

新行插哪儿：找同目录最后一个已列出的文件行，贴在它后面，缩进和注释列对齐它。
整个子目录在树里一个文件都没出现 → 不猜位置，跳过并报出来，人工决定。

和 tests/test_readme_tree.py 配对使用：这个脚本写，那个测试验。解析规则两边一致，
踩过的坑也一样（一行多文件、log_translations/ 故意不展开、__init__.py 不算），
详见那边的模块注释。
"""
import argparse
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"
READMES = ["README.md", "README_zh-CN.md", "README_JA.md"]

CURATED = {"tests"}
IGNORED = {"__init__.py"}

BLOCK_RE = re.compile(r"(^```text$)(.*?)(^```$)", re.M | re.S)
ENTRY_RE = re.compile(r"[├└]──")
PY_RE = re.compile(r"[\w.]+\.py\b")
# 拆出 `│   ├── ` 这样的前缀，和后面的内容
PREFIX_RE = re.compile(r"^(.*?[├└]──\s+)(.*)$")


def module_of(block):
    for line in block.splitlines():
        line = line.strip()
        if not line:
            continue
        m = re.fullmatch(r"([a-z_]+)/", line)
        return m.group(1) if m else None
    return None


def listed_py(block):
    names = set()
    for line in block.splitlines():
        if not ENTRY_RE.search(line):
            continue
        for name in PY_RE.findall(line.split("#")[0]):
            if "*" not in name and name not in IGNORED:
                names.add(name)
    return names


def actual_map(module_dir, listed):
    """basename -> 所在目录。子目录在树里一个文件都没出现就整个跳过。"""
    out = {p.name: module_dir for p in module_dir.glob("*.py")
           if p.name not in IGNORED}
    for sub in sorted(module_dir.iterdir()):
        if not sub.is_dir() or sub.name == "__pycache__":
            continue
        files = [p for p in sorted(sub.rglob("*.py"))
                 if "__pycache__" not in p.parts and p.name not in IGNORED]
        if {p.name for p in files} & listed:
            for p in files:
                out[p.name] = p.parent
    return out


def comment_col(line):
    i = line.find("#")
    return i if i > 0 else None


def sync_block(block, module_dir, log):
    lines = block.splitlines()
    listed = listed_py(block)
    amap = actual_map(module_dir, listed)

    missing = sorted(set(amap) - listed)
    extra = sorted(listed - set(amap))
    if not missing and not extra:
        return block, False

    # 1. 删掉已消失的文件
    out = []
    for line in lines:
        if not ENTRY_RE.search(line):
            out.append(line)
            continue
        head = line.split("#")[0]
        toks = [t for t in PY_RE.findall(head) if t not in IGNORED]
        gone = [t for t in toks if t in extra]
        if not gone:
            out.append(line)
            continue
        if len(gone) == len(toks):
            log.append("  - 删行 %s" % line.strip())
            continue
        # 一行多文件：只摘掉消失的那个 token
        new = line
        for t in gone:
            new = re.sub(r"\s*/\s*" + re.escape(t) + r"\b", "", new)
            new = re.sub(r"\b" + re.escape(t) + r"\s*/\s*", "", new)
        log.append("  ~ 改行 %s" % new.strip())
        out.append(new)

    # 2. 插入新文件，贴到同目录最后一行后面
    for name in missing:
        target_dir = amap[name]
        anchor = None
        for i, line in enumerate(out):
            if not ENTRY_RE.search(line):
                continue
            toks = [t for t in PY_RE.findall(line.split("#")[0])
                    if t not in IGNORED and t in amap]
            if toks and amap[toks[0]] == target_dir:
                anchor = i
        if anchor is None:
            log.append("  ! %s 所在目录 %s 在树里没有锚点，跳过"
                       % (name, target_dir.relative_to(MAIN)))
            continue

        m = PREFIX_RE.match(out[anchor])
        prefix = m.group(1)
        # 锚点本来是本组最后一行，新行接替它的 └──
        if "└──" in prefix:
            out[anchor] = out[anchor].replace("└──", "├──", 1)
        col = comment_col(out[anchor])
        entry = prefix + name
        # 名字比锚点的注释列还长时至少留两个空格，别让 # 贴上去
        pad = max(col, len(entry) + 2) if col else len(entry) + 2
        entry = entry.ljust(pad) + "# TODO: 待补充"
        out.insert(anchor + 1, entry)
        log.append("  + 插入 %s" % entry.strip())

    return "\n".join(out), True


def sync_readme(path):
    """处理一份 README，返回 (新文本, 是否有改动, 改动日志)。

    单独成函数，是为了让 repl 闭包捕获本函数的局部变量，而不是 main() 里的循环
    变量——后者正是 ruff B023 拦的那种坑。这里 sub 是同步调用，实际不会出错，
    但没必要留着。
    """
    text = io.open(path, encoding="utf-8").read()
    log = []

    def repl(m):
        open_, body, close = m.group(1), m.group(2), m.group(3)
        module = module_of(body)
        if module is None or module in CURATED:
            return m.group(0)
        module_dir = MAIN / module
        if not module_dir.is_dir():
            return m.group(0)
        new_body, changed = sync_block(body, module_dir, log)
        if not changed:
            return m.group(0)
        return open_ + new_body + "\n" + close

    new_text = BLOCK_RE.sub(repl, text)
    return new_text, new_text != text, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="落盘，默认只预览")
    args = ap.parse_args()

    changed_any = False
    for readme in READMES:
        path = ROOT / readme
        new_text, changed, log = sync_readme(path)
        if not changed:
            print("%s 无需改动" % readme)
            continue

        changed_any = True
        print("%s:" % readme)
        for line in log:
            print(line)
        if args.write:
            io.open(path, "w", encoding="utf-8", newline="\n").write(new_text)
            print("  → 已写入")

    if changed_any and not args.write:
        print("\n以上为预览。加 --write 落盘。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
