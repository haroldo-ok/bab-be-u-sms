#!/usr/bin/env python3
"""Bank-aware report for this project's slot-2 call-graph profiles.

The stock smstest call-graph profiler bank-qualifies slot-1 code only.
This project runs the parser and movement engine from slot 2, so the
local smstest variant in perf/tool-src also records mapper[3].  SDCC
uses the same key convention for banked symbols: bank<<16 | CPU address.

Usage:
  prof_bank.py dump.txt game.noi *.rst [--top N] [--tree FUNC]
               [--asm FUNC]
"""
import argparse
import bisect
import collections
import re
import sys

NOI_RE = re.compile(r"DEF\s+(\S+)\s+0x([0-9A-Fa-f]+)")
RST_LABEL_RE = re.compile(r"^\s+([0-9A-Fa-f]{8})\s+\d+\s+([A-Za-z_][A-Za-z0-9_]*):?:\s*$")
RST_INS_RE = re.compile(r"^\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f ]{2,14}?)\s+\[\s*\d+\]\s+\d+\s+(.*)$")


def load_symbols(noi_path, rst_paths):
    symbols = {}
    for line in open(noi_path, errors="replace"):
        m = NOI_RE.match(line.strip())
        if not m:
            continue
        name, addr = m.group(1).lstrip("_"), int(m.group(2), 16)
        # RAM/data symbols cannot own executed PCs.  A full banked key
        # can numerically exceed 0xC000, so test only its CPU part.
        if (addr & 0xFFFF) >= 0xC000 or "$" in name or name.startswith(("l_", "s_")):
            continue
        symbols.setdefault(addr, name)

    instruction_lines = {}
    for path in rst_paths:
        for line in open(path, errors="replace"):
            m = RST_LABEL_RE.match(line)
            if m:
                addr, name = int(m.group(1), 16), m.group(2).lstrip("_")
                if (addr & 0xFFFF) < 0xC000 and "$" not in name:
                    symbols[addr] = name
            m = RST_INS_RE.match(line)
            if m:
                addr = int(m.group(1), 16)
                if (addr & 0xFFFF) < 0xC000:
                    instruction_lines[addr] = m.group(3).strip()
    return symbols, instruction_lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("noi")
    ap.add_argument("rst", nargs="*")
    ap.add_argument("--top", type=int, default=35)
    ap.add_argument("--tree")
    ap.add_argument("--asm", dest="asm_func")
    args = ap.parse_args()

    symbols, instruction_lines = load_symbols(args.noi, args.rst)
    by_space = collections.defaultdict(list)
    for addr, name in symbols.items():
        space = addr >> 16 if addr > 0xFFFF else 0
        by_space[space].append((addr, name))
    sorted_spaces = {}
    for space, vals in by_space.items():
        vals.sort()
        sorted_spaces[space] = ([a for a, _ in vals], [n for _, n in vals])

    def owner(key):
        if key == 0x38:
            return "IRQ"
        if key == 0x66:
            return "NMI"
        if key == 0xFFFFFF:
            return "ROOT"
        space = key >> 16 if key > 0xFFFF else 0
        addrs, names = sorted_spaces.get(space, ([], []))
        i = bisect.bisect_right(addrs, key) - 1
        if i >= 0:
            return names[i]
        # Some helper calls land in the fixed area before the first
        # retained label (for example RST vectors).
        return f"?{key:06X}"

    total = 0
    exclusive_by_addr = {}
    exclusive = collections.Counter()
    inclusive = []
    edges = []
    for line in open(args.dump):
        p = line.split()
        if not p:
            continue
        if p[0] == "T":
            total = int(p[1])
        elif p[0] == "X":
            addr, cycles = int(p[1], 16), int(p[2])
            exclusive_by_addr[addr] = cycles
            exclusive[owner(addr)] += cycles
        elif p[0] == "I":
            inclusive.append((int(p[3]), int(p[2]), owner(int(p[1], 16)), int(p[1], 16)))
        elif p[0] == "E":
            edges.append((int(p[1], 16), int(p[2], 16), int(p[3]), int(p[4])))

    def fmt(cycles):
        return f"{cycles:10d} {100.0 * cycles / total:5.1f}%"

    print(f"total {total} cycles ({total / 59736:.2f} console frames)")
    print("\n== exclusive (self) ==")
    for name, cycles in exclusive.most_common(args.top):
        print(f"{name:<28} {fmt(cycles)}")

    print("\n== inclusive (outermost entry -> return) ==")
    # Multiple keys can resolve to the same displayed name only for
    # distinct internal entry points; keep them separate, as prof2 does.
    for cycles, calls, name, _addr in sorted(inclusive, reverse=True)[:args.top]:
        print(f"{name:<28} {fmt(cycles)}  calls {calls:7d}  {cycles / max(calls, 1):9.0f} cyc/call")

    if args.tree:
        children = collections.defaultdict(list)
        for parent, child, calls, cycles in edges:
            children[owner(parent)].append((cycles, calls, owner(child)))

        def show(func, depth, seen):
            agg = collections.defaultdict(lambda: [0, 0])
            for cycles, calls, child in children.get(func, []):
                agg[child][0] += cycles
                agg[child][1] += calls
            for child, (cycles, calls) in sorted(agg.items(), key=lambda kv: -kv[1][0]):
                if cycles < total * 0.003:
                    continue
                print("  " * depth + f"{child:<28} {fmt(cycles)}  calls {calls:7d}  {cycles / max(calls,1):9.0f} cyc/call")
                if depth < 5 and child not in seen:
                    show(child, depth + 1, seen | {child})

        print(f"\n== call tree from {args.tree} (edges >=0.3%) ==")
        show(args.tree, 1, {args.tree})

    if args.asm_func:
        starts = [a for a, n in symbols.items() if n == args.asm_func]
        if not starts:
            print(f"\nno symbol named {args.asm_func}", file=sys.stderr)
            return 2
        start = min(starts)
        space = start >> 16 if start > 0xFFFF else 0
        addrs, _names = sorted_spaces[space]
        i = addrs.index(start)
        end = addrs[i + 1] if i + 1 < len(addrs) else start + 0x4000
        print(f"\n== {args.asm_func} per instruction ({start:06X}..{end-1:06X}) ==")
        step = 0x10000 if start > 0xFFFF else 1
        # Address keys are dense in the CPU component even when banked.
        a = start
        while a < end:
            if a in instruction_lines:
                print(f"{a:06X} {exclusive_by_addr.get(a, 0):10d}  {instruction_lines[a]}")
            a += 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
