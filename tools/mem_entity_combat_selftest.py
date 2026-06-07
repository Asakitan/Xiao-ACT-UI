# -*- coding: utf-8 -*-
"""Offline selftest for EntityCombatReader (no live game).

Synthesises ZEntity -> attrs -> cacheSlim._values -> object[] of LongAttr objects
(klass name "LongAttr") + an _indexPart blob carrying HP/MAX_HP attr ids, and
verifies read_combat() recovers (CurHp, MaxHp) via the HP-pair invariant.

Run: python tools/mem_entity_combat_selftest.py
"""
from __future__ import annotations
import os, struct, sys
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from mem_probe.il2cpp.mem_entity_combat import EntityCombatReader, A_HP, A_MAX_HP


class FakeMem:
    def __init__(self, base=0x0000_0002_0000_0000, size=0x40000):
        self.base=base; self.buf=bytearray(size); self._next=base+0x100
    def alloc(self,n):
        a=(self._next+0xF)&~0xF; self._next=a+n; return a
    def _i(self,a): return a-self.base
    def wu64(self,a,v): struct.pack_into("<Q",self.buf,self._i(a),v&(2**64-1))
    def w64(self,a,v): struct.pack_into("<q",self.buf,self._i(a),v)
    def wu32(self,a,v): struct.pack_into("<I",self.buf,self._i(a),v&0xFFFFFFFF)
    def w32(self,a,v): struct.pack_into("<i",self.buf,self._i(a),v)
    def wbytes(self,a,b): self.buf[self._i(a):self._i(a)+len(b)]=b
    def read_bytes(self,a,n):
        i=self._i(a)
        if i<0 or i+n>len(self.buf): return None
        return bytes(self.buf[i:i+n])
    def read_u64(self,a): b=self.read_bytes(a,8); return struct.unpack("<Q",b)[0] if b else None
    def read_i64(self,a): b=self.read_bytes(a,8); return struct.unpack("<q",b)[0] if b else None
    def read_u32(self,a): b=self.read_bytes(a,4); return struct.unpack("<I",b)[0] if b else None
    def read_i32(self,a): b=self.read_bytes(a,4); return struct.unpack("<i",b)[0] if b else None


_fails=[]
def check(name,cond):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond: _fails.append(name)


def _mk_klass(m, name):
    sa=m.alloc(len(name)+1); m.wbytes(sa, name.encode()+b"\x00")
    k=m.alloc(0x40); m.wu64(k+0x10, sa)   # Il2CppClass.name @ +0x10
    return k

def _mk_longattr(m, klass, value):
    o=m.alloc(0x30); m.wu64(o, klass); m.w64(o+0x18, value)  # value_ @ +0x18
    return o


def build_entity(m, longs):
    KLA=_mk_klass(m,"LongAttr")
    n=len(longs)
    arr=m.alloc(0x20+(n+1)*8); m.wu32(arr+0x18, n)
    for j,v in enumerate(longs):
        m.wu64(arr+0x20+j*8, _mk_longattr(m,KLA,v))
    vals=m.alloc(0x40); m.wu64(vals+0x28, arr)         # _values[0].Item2
    ip=m.alloc(0x4000)                                  # _indexPart with HP ids
    m.wu32(ip+0x100, A_HP); m.wu32(ip+0x200, A_MAX_HP)
    attrs=m.alloc(0x40); m.wu64(attrs+0x18, ip); m.wu64(attrs+0x20, vals)
    ent=m.alloc(0x60); m.wu64(ent+0x48, attrs)
    return ent


def main():
    print("test_entity_combat_reader")
    m=FakeMem()
    ecr=EntityCombatReader(m)
    # dummy being hit: HP pair (CurHp 177503, MaxHp 1775031) + filler longs
    ent=build_entity(m, [12345, 177503, 1775031, 1780837993397])  # last = server-time (excluded)
    check("is_combat_entity True (HP ids in _indexPart)", ecr.is_combat_entity(ent))
    c=ecr.read_combat(ent)
    check("read_combat returns dict", c is not None)
    check("max_hp = 1775031", c and c["max_hp"]==1775031)
    check("cur_hp = 177503", c and c["cur_hp"]==177503)
    check("hp_pct ~0.10", c and abs(c["hp_pct"]-0.1)<0.001)

    # full HP: both equal
    ent2=build_entity(m, [1775031, 1775031])
    c2=ecr.read_combat(ent2)
    check("full HP -> 100%", c2 and c2["cur_hp"]==1775031 and c2["max_hp"]==1775031)

    # non-combat entity (no HP ids in _indexPart) -> None
    m2=FakeMem(); ecr2=EntityCombatReader(m2)
    KLA=_mk_klass(m2,"LongAttr")
    arr=m2.alloc(0x40); m2.wu32(arr+0x18,1); m2.wu64(arr+0x20,_mk_longattr(m2,KLA,500))
    vals=m2.alloc(0x40); m2.wu64(vals+0x28,arr)
    ip=m2.alloc(0x100)  # no HP ids
    attrs=m2.alloc(0x40); m2.wu64(attrs+0x18,ip); m2.wu64(attrs+0x20,vals)
    ent3=m2.alloc(0x60); m2.wu64(ent3+0x48,attrs)
    check("non-combat entity -> read_combat None", ecr2.read_combat(ent3) is None)

    print()
    if _fails: print(f"FAILED: {_fails}"); sys.exit(1)
    print("ALL PASS")


if __name__=="__main__":
    main()
