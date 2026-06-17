# -*- coding: utf-8 -*-
"""Targeted scan for the live combat HP-bar object (HudGmData) of the dummy.

Known: MaxHp ~1.78M, CurHp ~200-300K. HudGmData is inline at HudGm+0x70:
  State@HudGm+0x70  MaxHp@+0x78  CurHp@+0x80  MaxBreak@+0x88  CurBreak@+0x8C
  HateList@+0x90  uuid@+0xA0  configId@+0xA8 ; klass@+0x0.
Anchor on MaxHp in a narrow band, verify CurHp, then validate the klass by name.
"""
from __future__ import annotations
import argparse, struct, sys, os, time
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from plugins.star_resonance_plugin.mem.process import StarProcess
ESTATE={0:"default",1:"singing",2:"skill",9:"DEAD",10:"stiff",11:"swimstiff",12:"born"}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--max-lo",type=int,default=1_600_000)
    ap.add_argument("--max-hi",type=int,default=2_000_000)
    ap.add_argument("--cur-lo",type=int,default=50_000)
    ap.add_argument("--cur-hi",type=int,default=1_000_000)
    a=ap.parse_args()
    pm=StarProcess()
    def kname(kp):
        if not (0x10000<=(kp or 0)<=0x7FFFFFFFFFFF): return None
        np=pm.read_u64(kp+0x10)
        if not (0x10000<=(np or 0)<=0x7FFFFFFFFFFF): return None
        b=pm.read_bytes(np,48); return b.split(b"\x00",1)[0].decode("utf-8","replace") if b else None
    t0=time.time(); cands=[]
    for r in pm.iter_regions(only_readable=True, only_private=True):
        base=r.base; size=r.size; off=0
        while off<size:
            n=min(16*1024*1024, size-off)
            blob=pm.read_bytes(base+off,n)
            if blob is None: break
            L=len(blob); p=0; lim=L-8
            while p<=lim:
                mh=struct.unpack_from("<q",blob,p)[0]
                if a.max_lo<=mh<=a.max_hi and p+0x40<=L and p-0x80>=0:
                    ch=struct.unpack_from("<q",blob,p+8)[0]
                    if a.cur_lo<=ch<=mh and p-0x78>=0:
                        hg=base+off+p-0x78     # MaxHp = HudGm+0x78
                        cands.append(hg)
                p+=8
            off+=n
        if time.time()-t0>240: print("[cap]"); break
    print(f"[scan] {time.time()-t0:.0f}s, {len(cands)} HudGmData-shaped candidates")
    for hg in cands[:80]:
        klass=pm.read_u64(hg); nm=kname(klass)
        state=pm.read_i32(hg+0x70); mh=pm.read_i64(hg+0x78); ch=pm.read_i64(hg+0x80)
        maxbrk=pm.read_i32(hg+0x88); curbrk=pm.read_i32(hg+0x8C)
        uuid=pm.read_i64(hg+0xA0); cfg=pm.read_i32(hg+0xA8)
        pct=ch/mh*100 if mh else 0
        print(f"  HudGm@{hex(hg)} klass={hex(klass)}<{nm}> uuid={uuid} cfg={cfg} "
              f"state={ESTATE.get(state,state)} hp={ch}/{mh} ({pct:.0f}%) break={curbrk}/{maxbrk}")

if __name__=="__main__":
    main()
