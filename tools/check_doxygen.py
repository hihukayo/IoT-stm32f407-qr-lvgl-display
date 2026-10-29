#!/usr/bin/env python3
"""检查固件 C 源文件的 Doxygen 注释覆盖率。

用法（在仓库根目录）::

    python tools/check_doxygen.py            # 只统计，有缺口时退出码 1
    python tools/check_doxygen.py --list     # 列出每一个缺注释的函数

判定规则：
- 只看 ``firmware/Core/Src/*.c`` 与 ``firmware/Core/Inc/*.h``；
- 一个"函数定义"= 行首类型名 + 函数名 + 参数表，且后续 12 行内出现 ``{``
  （用于排除头文件里的原型声明）；
- 定义行上方（跳过空行与 ``__attribute__`` 之类的修饰行）若以 ``*/`` 结尾，
  且向上能找到 ``/**``，视为已有 Doxygen 块；
- 头文件里的**函数原型**（以 ``);`` 结尾且不带 ``=``）单独统计：
  要求上方紧邻一个注释块（``/*`` 或 ``/**`` 均可）。

返回值：0 = 全覆盖；1 = 存在缺口（可直接用于 CI 或 pre-commit）。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CORE_DIR = REPO_ROOT / "firmware" / "Core"

# 函数定义：行首（可带 static / inline），返回类型 + 名字 + 左括号
FUNC_RE = re.compile(
    r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t\*]*?\b(?P<name>[A-Za-z_]\w*)\s*\("
)
KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "case", "sizeof",
    "typedef", "struct", "union", "enum",
}


def block_above(lines: list[str], idx: int) -> bool:
    """判断 idx 行上方**紧邻**的注释块是否为 ``/** ... */``。

    只看紧挨着的那一个块：跳过空行与属性修饰行后，若上一行以 ``*/`` 结尾，
    就向上找该块的起始行，只有以 ``/*`` 开头（即 Doxygen 块）才算通过。
    早期版本会一路向上搜索，把上一个函数的注释误算给当前函数，导致漏判。
    """
    j = idx - 1
    while j >= 0 and (lines[j].strip() == "" or lines[j].strip().startswith("__")):
        j -= 1
    if j < 0 or not lines[j].strip().endswith("*/"):
        return False
    while j >= 0:
        stripped = lines[j].strip()
        if stripped.startswith("/*"):
            return stripped.startswith("/**")
        j -= 1
    return False


def comment_above(lines: list[str], idx: int) -> bool:
    """判断 idx 行上方是否紧邻任意注释块（头文件原型用）。"""
    j = idx - 1
    while j >= 0 and lines[j].strip() == "":
        j -= 1
    return j >= 0 and lines[j].strip().endswith("*/")


def scan_file(path: Path):
    """返回 (函数缺口列表, 原型缺口列表, 函数总数, 原型总数)。"""
    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    func_missing: list[tuple[int, str]] = []
    proto_missing: list[tuple[int, str]] = []
    func_total = proto_total = 0

    for i, line in enumerate(lines):
        if not line or line[0] in " \t#/}":
            continue

        stripped = line.strip()

        # 以 ); 结尾且无初始化表达式 = 原型/前置声明
        if stripped.endswith(");") and "=" not in stripped and not stripped.startswith("*"):
            if path.suffix == ".h":
                proto_total += 1
                if not comment_above(lines, i):
                    proto_missing.append((i + 1, stripped[:70]))
            continue  # .c 里的前置声明不要求注释

        m = FUNC_RE.match(line.rstrip())
        if not m:
            continue
        if m.group("name") in KEYWORDS:
            continue

        window = "\n".join(lines[i:i + 12])
        if "{" not in window:
            continue  # 多行原型或宏

        func_total += 1
        if not block_above(lines, i):
            func_missing.append((i + 1, line.strip()[:70]))

    return func_missing, proto_missing, func_total, proto_total


def main() -> int:
    parser = argparse.ArgumentParser(description="检查 Doxygen 注释覆盖率")
    parser.add_argument(
        "--list", action="store_true", help="逐条列出缺注释的函数/原型"
    )
    parser.add_argument(
        "--root", type=Path, default=CORE_DIR, help="扫描根目录（默认 firmware/Core）"
    )
    args = parser.parse_args()

    root: Path = args.root
    if not root.is_dir():
        print(f"目录不存在：{root}", file=sys.stderr)
        return 2

    files = sorted(list(root.glob("Src/*.c")) + list(root.glob("Inc/*.h")))
    if not files:
        print(f"在 {root} 下没有找到 .c/.h 文件", file=sys.stderr)
        return 2

    total_func = total_missing = 0
    total_proto = total_proto_missing = 0
    details: list[str] = []

    for path in files:
        func_missing, proto_missing, func_total, proto_total = scan_file(path)
        total_func += func_total
        total_missing += len(func_missing)
        total_proto += proto_total
        total_proto_missing += len(proto_missing)
        for ln, sig in func_missing:
            details.append(f"  {path.name}:{ln}  函数无 Doxygen 块   {sig}")
        for ln, sig in proto_missing:
            details.append(f"  {path.name}:{ln}  原型无注释块       {sig}")

    print(f"函数定义：{total_func} 个，缺 Doxygen 块 {total_missing} 个")
    print(f"头文件原型：{total_proto} 个，缺注释块 {total_proto_missing} 个")

    if details:
        if args.list:
            print("\n缺口明细：")
            print("\n".join(details))
        else:
            print("\n（加 --list 查看逐条明细）")
        return 1

    print("\n注释覆盖完整。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
