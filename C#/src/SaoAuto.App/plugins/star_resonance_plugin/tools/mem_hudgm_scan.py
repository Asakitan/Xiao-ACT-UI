# -*- coding: utf-8 -*-
"""Structural heap scan for HudGm objects (NO klass resolution needed).

HudGm layout: State@0x70(int) MaxHp@0x78(long) CurHp@0x80(long) MaxBreak@0x88(int)
CurBreak@0x8C(int) HateList@0x90(ptr) uuid@0xA0(long) configId@0xA8(int), klass@0x0.
We scan readable private regions for that signature -> the displayed combat HP of
every monster/boss/dummy, found purely by structure ("find that local block").
"""
from __future__ import annotations
import struct, sys, os, time
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from mem_probe.process import StarProcess
ESTATE={0:"default",1:"singing",2:"skill",9:"DEAD",10:"stiff",11:"swimstiff",12:"born"}

def main():
    pm=StarProcess()
    # il2cpp klass region (observed: CharSerialize@0x336f9b70, ZEntityMgr@0x33235ef0)
    KLO, KHI = 0x33000000, 0x36000000
    hits=[]; klass_count={}
    t0=time.time(); scanned=0
    for r in pm.iter_regions(only_readable=True, only_private=True):
        base=r.base; size=r.size
        off=0
        while off<size:
            n=min(16*1024*1024, size-off)
            blob=pm.read_bytes(base+off, n)
            if blob is None: break
            scanned+=n
            L=len(blob)
            # iterate candidate HudGm bases p (need p+0xB0 within blob)
            p=0
            lim=L-0xB0
            while p<=lim:
                maxhp=struct.unpack_from("<q", blob, p+0x78)[0]
                if 100000<=maxhp<=5_000_000_000:
                    curhp=struct.unpack_from("<q", blob, p+0x80)[0]
                    if 0<=curhp<=maxhp:
                        state=struct.unpack_from("<i", blob, p+0x70)[0]
                        cfg=struct.unpack_from("<i", blob, p+0xA8)[0]
                        klass=struct.unpack_from("<Q", blob, p+0x0)[0]
                        if 0<=state<=30 and 1<=cfg<=100_000_000 and KLO<=klass<=KHI:
                            maxbrk=struct.unpack_from("<i", blob, p+0x88)[0]
                            curbrk=struct.unpack_from("<i", blob, p+0x8C)[0]
                            uuid=struct.unpack_from("<q", blob, p+0xA0)[0]
                            if 0<=maxbrk<=100_000_000 and 0<=curbrk<=maxbrk if maxbrk else True:
                                hits.append((base+off+p,klass,state,maxhp,curhp,maxbrk,curbrk,uuid,cfg))
                                klass_count[klass]=klass_count.get(klass,0)+1
                p+=8
            off+=n
        if time.time()-t0>240:
            print("[scan] time cap hit"); break
    print(f"[scan] {scanned/1e9:.2f}GB in {time.time()-t0:.0f}s, {len(hits)} region-klass signature hits")
    # validate each unique klass by reading its il2cpp name (klass+0x10 -> cstring)
    def kname(kp):
        np=pm.read_u64(kp+0x10)
        if not (0x10000<=(np or 0)<=0x7FFFFFFFFFFF): return None
        b=pm.read_bytes(np,48);
        return b.split(b"\x00",1)[0].decode("utf-8","replace") if b else None
    names={k:kname(k) for k in klass_count}
    named=[(k,c,names[k]) for k,c in klass_count.items() if names[k]]
    named.sort(key=lambda x:-x[1])
    print("named klasses (addr,count,name):")
    for k,c,nm in named[:15]:
        print(f"  {hex(k)} x{c} -> {nm}")
    good=set(k for k,nm in names.items() if nm and ("HudGm" in nm or "Hud" in nm))
    print(f"\nHudGm-like klasses: {[hex(k) for k in good]}")
    shown=0
    for addr,klass,state,maxhp,curhp,maxbrk,curbrk,uuid,cfg in hits:
        if good and klass not in good: continue
        if not good and names.get(klass) is None: continue
        pct=curhp/maxhp*100 if maxhp else 0
        print(f"  @{hex(addr)} klass={hex(klass)}<{names.get(klass)}> uuid={uuid} cfg={cfg} "
              f"state={ESTATE.get(state,state)} hp={curhp}/{maxhp} ({pct:.0f}%) break={curbrk}/{maxbrk}")
        shown+=1
        if shown>=60: break

if __name__=="__main__":
    main()
