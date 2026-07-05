# 诊断: 打印 HP 周围 ±256 字节每个 8 字节槽的分类 (零 / 模块指针 / 堆指针 / 小整数).
from __future__ import annotations
import bisect, json, os, sys

# sao_auto root: .. (parent of mem_probe)
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)
from .process import StarProcess

ANCHORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")


def main() -> int:
    anchors = json.load(open(ANCHORS, encoding="utf-8"))
    hp = int(anchors["self_hp_addr"], 16)
    pm = StarProcess()
    try:
        mods = pm.list_modules()
        idx = sorted([(m.base, m.base + m.size, m.name) for m in mods])
        bases = [b for b, _, _ in idx]
        def classify(v: int):
            if v == 0: return "zero"
            if v < 0x10000: return f"int({v})"
            if v >= 0x7FFFFFFFFFFF: return "kernel?"
            i = bisect.bisect_right(bases, v) - 1
            if i >= 0:
                b, e, n = idx[i]
                if b <= v < e:
                    return f"MOD {n}+0x{v-b:X}"
            return "heap"

        before, after = 0x100, 0x100
        base = hp - before
        blob = pm.read_bytes(base, before + after)
        if blob is None:
            print("read fail"); return 1
        print(f"hp = 0x{hp:X}")
        print(f"模块数: {len(mods)}, 范围: 0x{idx[0][0]:X} ~ 0x{idx[-1][1]:X}")
        for i in range(0, len(blob), 8):
            v = int.from_bytes(blob[i:i+8], "little")
            off = i - before
            mark = "  <-- HP" if off == 0 else ("  <-- MaxHP" if off == -0x20 else "")
            print(f"  [{off:+#06x}] {v:016X}  {classify(v)}{mark}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    sys.exit(main())
