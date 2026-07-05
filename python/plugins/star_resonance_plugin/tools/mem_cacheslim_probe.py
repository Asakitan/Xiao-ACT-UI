# -*- coding: utf-8 -*-
# Live probe: decode ZAttrCacheSlim by locating the ordered attr-id keys.
#
# ZAttr<T> = { bool isDefault_; T value_; object bindWatchers_ } after the 0x10
# il2cpp header -> value_ at obj+0x14 (4B types) / obj+0x18 (8B types/refs).
# The object lacks an Id, so object[] order maps to keys via _indexPart. This
# probe dumps a target entity's typed attr array and scans _indexPart for known
# attr ids to find the keys layout.
from __future__ import annotations
import struct, sys, os
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import EntityMgrReader

PLAUS=lambda p: bool(p and 0x10000<=p<=0x7FFFFFFFFFFF)
KNOWN={11310:"HP",11320:"MAX_HP",440:"MAX_EXT",441:"EXT",442:"MAX_STUN",443:"STUN",
       444:"OVERDRIVE",455:"BREAK_STAGE",100:"SKILL_ID",471:"HATED_ID",11830:"STUN_DMG%"}

def kname(pm,kp):
    try:
        np=pm.read_u64(kp+0x10)
        if not PLAUS(np): return "?"
        b=pm.read_bytes(np,40); return b.split(b"\x00",1)[0].decode("utf-8","replace") if b else "?"
    except Exception: return "?"

def read_attr_value(pm,o):
    okl=pm.read_u64(o); nm=kname(pm,okl)
    if nm=="LongAttr": return ("long",pm.read_i64(o+0x18))
    if nm=="IntAttr": return ("int",pm.read_i32(o+0x14))
    if nm=="FloatAttr": return ("float",round(struct.unpack("<f",pm.read_bytes(o+0x14,4))[0],3))
    if nm=="BoolAttr": return ("bool",pm.read_bytes(o+0x14,1)[0])
    if nm=="StringAttr":
        sp=pm.read_u64(o+0x18);
        if PLAUS(sp):
            ln=pm.read_i32(sp+0x10)
            if ln and 0<ln<=64: return ("str",pm.read_utf16(sp+0x14,ln))
        return ("str","")
    return (nm,pm.read_i64(o+0x14))

def main():
    src=StaticDpsSource(); sr=src.sr; pm=sr.pm
    si=ScriptIndex.load(os.path.join(_ROOT,"mem_probe","il2cpp","out","7068f8aa","dumper_out","script.json"))
    klass=pm.read_u64(sr.ga+si.find_klass("Panda.ZGame.ZEntityMgr"))
    emr=EntityMgrReader(src); emr._resolve_mgr_klass=lambda:klass; emr._mgr_addr=0
    mgr=emr.locate(0,force_rescan=True); puid=pm.read_i64(mgr+0x10)
    print("mgr",hex(mgr),"playerUuid_",puid)
    # pick the entity with the most attrs (player) + any big-HP one
    targets=[]
    d=pm.read_u64(mgr+0x28)
    for key,ent in emr._read_dict_entries(d,64):
        attrs=pm.read_u64(ent+0x48)
        if not PLAUS(attrs): continue
        vals=pm.read_u64(attrs+0x20)
        if not PLAUS(vals): continue
        arr=pm.read_u64(vals+0x20+8)            # _values[0].Item2
        al=pm.read_u32(arr+0x18) if PLAUS(arr) else 0
        targets.append((al,ent,attrs,vals,arr,pm.read_i64(ent+0xC0),pm.read_i64(ent+0xC8)))
    targets.sort(reverse=True)
    for al,ent,attrs,vals,arr,uuid,cfg in targets[:3]:
        ip=pm.read_u64(attrs+0x18)
        print(f"\n=== ent@{hex(ent)} uuid={uuid} cfg={cfg} arrlen={al} _indexPart={hex(ip)} ===")
        print("  object[] typed values:")
        for j in range(min(al,40)):
            o=pm.read_u64(arr+0x20+j*8)
            if not PLAUS(o): print(f"   [{j:2d}] null"); continue
            t,v=read_attr_value(pm,o)
            print(f"   [{j:2d}] {t:6s} = {v}")
        # scan _indexPart region for known attr ids (uint32)
        if PLAUS(ip):
            blob=pm.read_bytes(ip,0x4000)
            if blob:
                found=[]
                for off in range(0,len(blob)-4,4):
                    v=struct.unpack_from("<I",blob,off)[0]
                    if v in KNOWN: found.append((off,v,KNOWN[v]))
                print(f"  _indexPart known-attr-id hits ({len(found)}):", found[:40])

if __name__=="__main__":
    main()
