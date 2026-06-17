# -*- coding: utf-8 -*-
"""Find the dummy entity (MaxHp ~1.78M) in ZEntityMgr and dump ALL its typed attrs.

Combat HP lives in ZEntity.attrs_ (ZAttrCollection) -> cacheSlim_._values @ attrs+0x20
(ValueTuple<uint,object[]>[]). Each object[] element is a typed ZAttr<T>:
value_ @ obj+0x14 (Int/Float/Bool), @ obj+0x18 (Long/String). We dump every slot so
MaxHp(=~1.78M) and CurHp(=~200-300K) are visible, plus the matching slot indices.
"""
from __future__ import annotations
import struct, sys, os
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from plugins.star_resonance_plugin.mem.process import StarProcess
from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import EntityMgrReader
P=lambda p: bool(p and 0x10000<=p<=0x7FFFFFFFFFFF)

def kname(pm,kp):
    np=pm.read_u64(kp+0x10) if P(kp) else 0
    if not P(np): return "?"
    b=pm.read_bytes(np,40); return b.split(b"\x00",1)[0].decode("utf-8","replace") if b else "?"

def read_val(pm,o):
    t=kname(pm,pm.read_u64(o))
    if t=="LongAttr": return ("long",pm.read_i64(o+0x18))
    if t=="IntAttr": return ("int",pm.read_i32(o+0x14))
    if t=="FloatAttr": return ("float",round(struct.unpack("<f",pm.read_bytes(o+0x14,4))[0],3))
    if t=="BoolAttr": return ("bool",pm.read_bytes(o+0x14,1)[0])
    if t=="StringAttr":
        sp=pm.read_u64(o+0x18)
        if P(sp):
            ln=pm.read_i32(sp+0x10)
            if ln and 0<ln<=64: return ("str",pm.read_utf16(sp+0x14,ln))
        return ("str","")
    return (t,pm.read_i64(o+0x14))

def main():
    pm=StarProcess()
    src=StaticDpsSource(); sr=src.sr
    si=ScriptIndex.load(os.path.join(_ROOT,"mem_probe","il2cpp","out","7068f8aa","dumper_out","script.json"))
    klass=pm.read_u64(sr.ga+si.find_klass("Panda.ZGame.ZEntityMgr"))
    emr=EntityMgrReader(src); emr._resolve_mgr_klass=lambda:klass; emr._mgr_addr=0
    mgr=emr.locate(0,force_rescan=True); print("mgr",hex(mgr))
    seen=set()
    for nm,off in (("entity",0x28),("monster",0x68),("boss",0x60)):
        d=pm.read_u64(mgr+off)
        for key,ent in emr._read_dict_entries(d,64):
            if ent in seen: continue
            seen.add(ent)
            attrs=pm.read_u64(ent+0x48)
            if not P(attrs): continue
            vals=pm.read_u64(attrs+0x20)
            if not P(vals): continue
            arr=pm.read_u64(vals+0x28); al=pm.read_u32(arr+0x18) if P(arr) else 0
            slots=[]
            has_dummy_hp=False
            for j in range(min(al,40)):
                o=pm.read_u64(arr+0x20+j*8)
                if not P(o): slots.append((j,"null",None)); continue
                t,v=read_val(pm,o); slots.append((j,t,v))
                if t=="long" and isinstance(v,int) and 1_600_000<=v<=2_000_000:
                    has_dummy_hp=True
            if has_dummy_hp:
                uuid=pm.read_i64(ent+0xC0); cfg=pm.read_i64(ent+0xC8)
                print(f"\n=== {nm} ent@{hex(ent)} uuid={uuid} cfg={cfg} (MaxHp~1.78M FOUND) ===")
                for j,t,v in slots:
                    mark=""
                    if t=="long" and isinstance(v,int):
                        if 1_600_000<=v<=2_000_000: mark=" <== MAX_HP?"
                        elif 50_000<=v<=600_000: mark=" <== CUR_HP?"
                    print(f"   [{j:2d}] {t:6s} = {v}{mark}")

if __name__=="__main__":
    main()
