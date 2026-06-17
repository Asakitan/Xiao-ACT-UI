# -*- coding: utf-8 -*-
"""Live probe: read displayed combat HP/break/state via Panda.Hud.HudGmRender.

Resolves classes by NAME via Il2CppMetadataRegistration.types[] (the il2cpp global
type table) -> works for HudGm/HudGmRender which aren't in script.json's TypeInfo
list. Version-robust except the MR address, which is passed in (from the dumper:
MetadataRegistration VA 0x1888b20d0 - image base 0x180000000 = RVA 0x88b20d0).

Structural path:
  HudGmRender (ZSingleton)  hudGmDict_ : ZDictionary<long, HudGm> @ +0x10
  HudGm  gmData_@0x70 -> State@0x70 MaxHp@0x78 CurHp@0x80 MaxBreak@0x88 CurBreak@0x8C
         uuid_@0xA0  configId_@0xA8
ZDictionary<long,V>: entries_@+0x20, count_@+0x38; Entry{hash@0,next@4,key@8,val@0x10,sz0x18}
"""
from __future__ import annotations
import argparse, sys, os
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from plugins.star_resonance_plugin.mem.process import StarProcess
import mem_probe.il2cpp.metadata_registration as M
P=lambda p: bool(p and 0x10000<=p<=0x7FFFFFFFFFFF)
ESTATE={0:"default",1:"singing",2:"skill",9:"DEAD",10:"stiff",11:"swimstiff",12:"born"}

def find_klasses(pm, mr, wanted):
    found={w:0 for w in wanted}
    for e in M.iter_types(pm, mr, only_class=True):
        k=e.data_value
        if not P(k): continue
        head=pm.read_bytes(k,0x20)
        if not head: continue
        nptr=int.from_bytes(head[0x10:0x18],"little")
        nm=M._read_cstring(pm,nptr)
        if nm in found and not found[nm]:
            found[nm]=k
            if all(found.values()): break
    return found

def walk_dict(pm, dct):
    entries=pm.read_u64(dct+0x20); count=pm.read_i32(dct+0x38)
    out=[]
    if not P(entries) or not (0<=(count or 0)<=8192): return out
    alen=pm.read_u32(entries+0x18) or 0
    for i in range(min(alen,1024)):
        ep=entries+0x20+i*0x18
        h=pm.read_i32(ep)
        if h is None or h<0: continue
        hg=pm.read_u64(ep+0x10)
        if P(hg): out.append(hg)
    return out

def read_hudgm(pm,hg):
    return dict(state=pm.read_i32(hg+0x70), maxhp=pm.read_i64(hg+0x78), curhp=pm.read_i64(hg+0x80),
                maxbrk=pm.read_i32(hg+0x88), curbrk=pm.read_i32(hg+0x8C),
                uuid=pm.read_i64(hg+0xA0), cfg=pm.read_i32(hg+0xA8))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--mr-rva",type=lambda x:int(x,0),default=0x88b20d0)
    args=ap.parse_args()
    pm=StarProcess()
    M.METADATA_REGISTRATION_RVA=args.mr_rva
    mr=M.parse_metadata_registration(pm)
    if not (1000<=mr.types_count<=2000000):
        print(f"[!] types_count={mr.types_count} looks wrong; MR RVA likely off"); return 1
    ks=find_klasses(pm, mr, ("HudGmRender","HudGm"))
    print("klasses:",{k:hex(v) for k,v in ks.items()})
    hgr=ks.get("HudGmRender"); hgk=ks.get("HudGm")
    hudgms=[]
    if hgr:
        from plugins.star_resonance_plugin.mem.il2cpp.resolver import Il2CppResolver
        insts=Il2CppResolver(pm).locate_instances(hgr, max_hits=16)
        print(f"HudGmRender instances: {len(insts)}")
        for inst in insts:
            dct=pm.read_u64(inst+0x10)
            if P(dct): hudgms+=walk_dict(pm,dct)
    if not hudgms and hgk:
        print("falling back to HudGm instance scan")
        from plugins.star_resonance_plugin.mem.il2cpp.resolver import Il2CppResolver
        hudgms=Il2CppResolver(pm).locate_instances(hgk, max_hits=256)
    print(f"HudGm objects: {len(hudgms)}")
    for hg in hudgms[:50]:
        d=read_hudgm(pm,hg)
        if not d["maxhp"]: continue
        pct=(d["curhp"]/d["maxhp"]*100) if d["maxhp"] else 0
        print(f"  uuid={d['uuid']} cfg={d['cfg']} state={ESTATE.get(d['state'],d['state'])} "
              f"hp={d['curhp']}/{d['maxhp']} ({pct:.0f}%) break={d['curbrk']}/{d['maxbrk']}")
    return 0

if __name__=="__main__":
    raise SystemExit(main())
