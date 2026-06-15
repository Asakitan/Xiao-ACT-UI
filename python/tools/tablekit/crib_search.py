"""crib_search - 在容器/资源文件里搜已知中文名(明文对照), 定位 Bokura 表数据二进制位置.

只读, mmap. 支持 UTF-8 与 UTF-16LE 两种编码搜索。

用法:
    python -m tools.tablekit.crib_search --file "...\\m0.pkg" --needles 木桩 场地标记01
"""
from __future__ import annotations
import argparse, mmap, os


def hexdump(b, base=0):
    out = []
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        hx = " ".join("%02X" % c for c in chunk)
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        out.append("    %08X  %-47s  %s" % (base+i, hx, asc))
    return "\n".join(out)


def search(path, needles, max_hits=8, ctx=48):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            for nd in needles:
                for enc in ("utf-8", "utf-16-le"):
                    pat = nd.encode(enc)
                    hits = []
                    start = 0
                    while len(hits) < max_hits:
                        i = mm.find(pat, start)
                        if i < 0:
                            break
                        hits.append(i)
                        start = i + 1
                    print("\n[%s | %s] '%s' -> %d hit(s)%s"
                          % (os.path.basename(path), enc, nd, len(hits),
                             "" if hits else " (none)"))
                    for i in hits[:3]:
                        lo = max(0, i - ctx)
                        hi = min(size, i + len(pat) + ctx)
                        print("  @0x%X:" % i)
                        print(hexdump(mm[lo:hi], lo))
        finally:
            mm.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--needles", nargs="+", required=True)
    args = ap.parse_args()
    print("=== %s (%d bytes) ===" % (args.file, os.path.getsize(args.file)))
    search(args.file, args.needles)


if __name__ == "__main__":
    main()
