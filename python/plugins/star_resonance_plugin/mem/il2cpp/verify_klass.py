"""verify_klass — 用 script.json 的 *_TypeInfo RVA 取出 klass_ptr,
并在已知 HP 候选周围扫描这个 klass_ptr, 看对象首字节假设是否成立.

用法:
    python -m tools.mem_probe.il2cpp.verify_klass

读取:
    sao_auto/tools/mem_probe/il2cpp/out/<dump_id>/dumper_out/script.json
    (硬编码 dump_id = ef9ef95a, 也可改 --dump-id)
    GameAssembly.dll @ Star.exe
    Phase 0.1 找到的 4 个 HP 候选地址
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Dict, List

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from ..process import StarProcess

_DEFAULT_DUMP_ID = "ef9ef95a"
_HP_CANDIDATES = [
    0x4FE3A6F18,
    0x12023AC3D0,
    0x1202CDF0A0,
    0x13FBE4F670,
]
# 对每个 HP 候选, 检查 [-0x200, +0x80] 范围内每个 qword 是否等于 klass_ptr
_BACK_RANGE = 0x200
_FWD_RANGE = 0x80

# 我们关心的关键类 (从 dump.cs 找到的 HP-bearing 类)
_KEY_CLASSES = [
    "Zproto.UserFightAttr_TypeInfo",         # CurHp@0x10, MaxHp@0x18
    "Zproto.AvatarInfo_TypeInfo",            # 玩家头像信息
    "Panda.Hud.HudGmData_TypeInfo",          # GM HUD struct (struct, not class)
    "Panda.ZGame.DamagedBodyPartInfo_TypeInfo",
    "Zproto.ActorBodyPartInfo_TypeInfo",
    "PlayerMng_TypeInfo",
]


def _dumper_dir(dump_id: str) -> str:
    return os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "out", dump_id, "dumper_out",
    )


def main(argv=None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--dump-id", default=_DEFAULT_DUMP_ID)
    args = p.parse_args(argv)

    sj_path = os.path.join(_dumper_dir(args.dump_id), "script.json")
    print(f"[load] {sj_path}")
    with open(sj_path, "r", encoding="utf-8") as f:
        sj = json.load(f)

    # 索引 ScriptMetadata (按 Name)
    md = {e["Name"]: e for e in sj["ScriptMetadata"]}
    print(f"[load] {len(md)} metadata entries")

    pm = StarProcess()
    try:
        ga = next((m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll"), None)
        assert ga, "GameAssembly.dll 未加载"
        ga_base, ga_size = ga.base, ga.size
        print(f"[ga] base=0x{ga_base:X} size=0x{ga_size:X}")

        # 1. 取每个目标类的 klass_ptr
        klass_table: Dict[str, int] = {}
        for name in _KEY_CLASSES:
            entry = md.get(name)
            if entry is None:
                # 去掉命名空间前缀再试
                short = name.split(".")[-1]
                entry = md.get(short)
            if entry is None:
                print(f"[miss] {name}: 未在 ScriptMetadata 找到")
                continue
            rva = entry["Address"]
            ptr_addr = ga_base + rva
            klass_ptr = pm.read_u64(ptr_addr)
            print(f"[klass] {name}: RVA=0x{rva:X} addr=0x{ptr_addr:X} -> klass=0x{klass_ptr:X}")
            if klass_ptr and ga_base <= klass_ptr < ga_base + ga_size:
                klass_table[name] = klass_ptr
                # 读 klass 名称字节 (Il2CppClass.name @ +0x10)
                name_ptr = pm.read_u64(klass_ptr + 0x10)
                if name_ptr:
                    name_bytes = pm.read_bytes(name_ptr, 64)
                    if name_bytes:
                        s = name_bytes.split(b"\x00", 1)[0].decode("ascii", errors="replace")
                        print(f"        klass.name @ +0x10 -> '{s}'")
            else:
                print(f"        ⚠ klass_ptr 不在 GA 范围, 可能不是 _TypeInfo")

        # 2. 在 4 个 HP 候选周围 [-0x200, +0x80] 扫 klass_ptr 是否出现
        print(f"\n[scan] 在每个 HP 候选周围 [-0x{_BACK_RANGE:X}, +0x{_FWD_RANGE:X}] 找 klass_ptr...")
        for hp_addr in _HP_CANDIDATES:
            print(f"\n  -- hp@0x{hp_addr:X} --")
            base = hp_addr - _BACK_RANGE
            end = hp_addr + _FWD_RANGE
            blob = pm.read_bytes(base, end - base)
            if blob is None:
                print("    read failed")
                continue
            n = (len(blob) // 8) * 8
            hits = []
            for i in range(0, n, 8):
                v = int.from_bytes(blob[i:i+8], "little")
                for cname, kptr in klass_table.items():
                    if v == kptr:
                        off = base + i - hp_addr
                        hits.append((off, cname))
            if hits:
                for off, cname in hits:
                    print(f"    HIT {cname} @ hp{off:+#x}")
            else:
                print(f"    无 klass_ptr 命中 ({len(klass_table)} 类)")

        # 3. 反过来: 把 4 个 HP 候选 - 0x10 (CurHp 偏移) 处的对象 base, 读它的首 qword,
        #    检查这个首 qword 是不是 GA 内某个 _TypeInfo 解引用值
        print(f"\n[reverse] hp_addr - 0x10 处对象 base 的首 qword (klass-at-obj+0 假设)")
        for hp_addr in _HP_CANDIDATES:
            obj_base = hp_addr - 0x10  # 假设 CurHp 在 obj+0x10
            first_qw = pm.read_u64(obj_base)
            in_ga = ga_base <= (first_qw or 0) < ga_base + ga_size
            tag = " (in GA!)" if in_ga else ""
            print(f"  obj_base=0x{obj_base:X}: *obj=0x{first_qw:X}{tag}")
            if in_ga:
                # 这是个 klass_ptr! 尝试反查名字
                name_ptr = pm.read_u64(first_qw + 0x10)
                if name_ptr:
                    nb = pm.read_bytes(name_ptr, 64)
                    if nb:
                        s = nb.split(b"\x00", 1)[0].decode("ascii", errors="replace")
                        print(f"        klass.name = '{s}'")

        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    raise SystemExit(main())

