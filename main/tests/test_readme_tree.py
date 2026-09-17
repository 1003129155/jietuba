# -*- coding: utf-8 -*-
"""
README 目录树与真实代码结构的一致性检查。

只检查、不生成。生成方案不划算：树里真正有价值的是
`# CanvasScene — 画布场景，继承 QGraphicsScene` 这类手写描述，脚本产生不了；
想保住它就得另存一份三语描述表，等于把手工维护换个地方做，还多一层同步。
而且生成器要往 markdown 里回写，出 bug 是静默改坏 README；检查器只读，
出 bug 最多是 CI 假红，吵但不伤。

按 basename 比对，不解析缩进层级。canvas/ 和 canvas/items/ 的文件混在一个集合里
比，代价是同名文件跨子目录移动看不出来（实际没这种情况），换来解析逻辑少一大半
——这个测试自己的 bug 面越小越好。

三处踩过的坑，都是树的写法不规则，不是 README 真错了：
- `buttons.py / cards.py / icons.py` 一行列多个文件，所以按行抓全部 .py token，
  不能只抓第一个
- `core/` 把 `log_translations/` 当叶子列，不展开里面 13 个文件。所以子目录要不要
  递归进去，看树里有没有列过它的文件：一个都没列 = 故意不展开，整个跳过
- `app_*.xml.qm` 这种通配符不是文件名，跳过

`__init__.py` 整个忽略：它在树里是零信息（英文版索性都没列），而且每个包里都有，
会让上面那条「子目录有没有被列过」的判断永远命中，白白递归进不该展开的子目录。

tests/ 块是故意只列代表性测试（18 个 vs 实际 89 个），不参与检查。
末尾 MIN_CHECKED 兜底，防止解析器哪天退化成什么都没查还一路绿。
"""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"
READMES = ["README.md", "README_zh-CN.md", "README_JA.md"]

# 故意只列代表性文件、不求全的块
CURATED = {"tests"}

# 树里不写它，比对时也不算
IGNORED = {"__init__.py"}

# 每份 README 至少要有这么多个块被真正检查到，否则视为解析器失效
MIN_CHECKED = 10

BLOCK_RE = re.compile(r"^```text$(.*?)^```$", re.M | re.S)
ENTRY_RE = re.compile(r"[├└]──")
PY_RE = re.compile(r"[\w.]+\.py\b")


def _module_of(block):
    """块首行形如 `canvas/` 才认，否则返回 None 表示看不懂、跳过。"""
    for line in block.splitlines():
        line = line.strip()
        if not line:
            continue
        m = re.fullmatch(r"([a-z_]+)/", line)
        return m.group(1) if m else None
    return None


def _listed_py(block):
    """块里列出的所有 .py 文件名（一行可能有多个）。"""
    names = set()
    for line in block.splitlines():
        if not ENTRY_RE.search(line):
            continue
        head = line.split("#")[0]
        for name in PY_RE.findall(head):
            if "*" not in name and name not in IGNORED:
                names.add(name)
    return names


def _actual_py(module_dir, listed):
    """模块下的 .py。子目录在树里一个文件都没出现 = 故意没展开，整个跳过。"""
    names = {p.name for p in module_dir.glob("*.py")} - IGNORED
    for sub in module_dir.iterdir():
        if not sub.is_dir() or sub.name == "__pycache__":
            continue
        sub_names = {p.name for p in sub.rglob("*.py")
                     if "__pycache__" not in p.parts} - IGNORED
        if sub_names & listed:
            names |= sub_names
    return names


@pytest.mark.parametrize("readme", READMES)
def test_readme_tree_matches_code(readme):
    text = (ROOT / readme).read_text(encoding="utf-8")

    checked = []
    problems = []

    for block in BLOCK_RE.findall(text):
        module = _module_of(block)
        if module is None or module in CURATED:
            continue
        module_dir = MAIN / module
        if not module_dir.is_dir():
            continue

        listed = _listed_py(block)
        if not listed:
            continue

        actual = _actual_py(module_dir, listed)
        checked.append(module)

        missing = sorted(actual - listed)
        extra = sorted(listed - actual)
        if missing:
            problems.append("%s/ 树里缺少：%s" % (module, "、".join(missing)))
        if extra:
            problems.append("%s/ 树里多出（已删除或改名）：%s" % (module, "、".join(extra)))

    assert len(checked) >= MIN_CHECKED, (
        "%s 只解析出 %d 个模块块（应至少 %d 个），解析器可能已失效"
        % (readme, len(checked), MIN_CHECKED)
    )
    assert not problems, "%s 与代码结构不一致：\n  %s" % (readme, "\n  ".join(problems))
