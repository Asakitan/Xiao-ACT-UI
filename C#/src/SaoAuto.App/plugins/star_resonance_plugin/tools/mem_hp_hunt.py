# -*- coding: utf-8 -*-
"""Live HP hunt — find a known boss/dummy HP value inside ZAttrCacheSlim._values.

ZAttrCollection (attrs):
  +0x18  ZAttrCacheSlim _indexPart (IntPtr)
  +0x20  ValueTuple<uint, object[]>[] _values   (array: len@+0x18, elems@+0x20)
Each _values element (16B): Item1 uint @+0, Item2 object[] @+8.
Each object[] element is an attr value object; we scan it for the target HP.

Usage: python tools/mem_hp_hunt.py --lo 16000000 --hi 19500000
"""
from __future__ import annotations
import argparse, struct, sys, os
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import EntityMgrReader

PLAUS = lambda p: bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--lo",type=int,default=16000000)
    ap.add_argument("--hi",type=int,default=19500000); args=ap.parse_args()
    src=StaticDpsSource(); sr=src.sr; pm=sr.pm
    si=ScriptIndex.load(os.path.join(_ROOT,"mem_probe","il2cpp","out","7068f8aa","dumper_out","script.json"))
    klass=pm.read_u64(sr.ga+si.find_klass("Panda.ZGame.ZEntityMgr"))
    emr=EntityMgrReader(src); emr._resolve_mgr_klass=lambda:klass; emr._mgr_addr=0
    mgr=emr.locate(0,force_rescan=True); print("mgr",hex(mgr))
    lo,hi=args.lo,args.hi
    seen=set()
    for nm,off in (("entity",0x28),("monster",0x68)):
        d=pm.read_u64(mgr+off)
        for key,ent in emr._read_dict_entries(d,64):
            if ent in seen: continue
            seen.add(ent)
            attrs=pm.read_u64(ent+0x48)
            if not PLAUS(attrs): continue
            uuid=pm.read_i64(ent+0xC0); cfg=pm.read_i64(ent+0xC8)
            vals=pm.read_u64(attrs+0x20)
            if not PLAUS(vals): continue
            vlen=pm.read_u32(vals+0x18) or 0
            hits=[]
            structure=[]
            for i in range(min(vlen,32)):
                elem=vals+0x20+i*16
                k=pm.read_u32(elem+0); arr=pm.read_u64(elem+8)
                alen=pm.read_u32(arr+0x18) if PLAUS(arr) else 0
                structure.append((k,alen))
                if not PLAUS(arr): continue
                for j in range(min(alen,128)):
                    o=pm.read_u64(arr+0x20+j*8)
                    if not PLAUS(o): continue
                    blob=pm.read_bytes(o,0x40)
                    if not blob: continue
                    okl=struct.unpack_from("<Q",blob,0)[0]
                    for boff in range(0x10,0x38,4):
                        v64=struct.unpack_from("<q",blob,boff)[0]
                        v32=struct.unpack_from("<i",blob,boff)[0]
                        if lo<=v64<=hi:
                            hits.append((k,i,j,hex(o),hex(okl),boff,v64,'i64'))
                        elif lo<=v32<=hi:
                            hits.append((k,i,j,hex(o),hex(okl),boff,v32,'i32'))
            if hits:
                print(f"\n*** {nm} ent@{hex(ent)} uuid={uuid} cfg={cfg} vlen={vlen} struct(key,arrlen)={structure}")
                for h in hits:
                    print(f"    key={h[0]} valsIdx={h[1]} arrIdx={h[2]} obj={h[3]} objklass={h[4]} +{hex(h[5])} = {h[6]} ({h[7]})")


if __name__=="__main__":
    main()
