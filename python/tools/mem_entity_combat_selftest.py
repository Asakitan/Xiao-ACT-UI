# -*- coding: utf-8 -*-
"""Offline selftest for EntityCombatReader (no live game).

Synthesises the REAL ZAttrCacheSlim layout the production decoder walks:

    ZEntity.attrs_ -> ZAttrCollection.cacheSlim_
      _indexPart (Burst block): KeySegment*[32] @+0x10 (uint Keys[8] + int Count
        @+0x20 each), parallel int*[32] valueIndices @+0x110; segments live
        INSIDE the indexPart allocation (mirrors the live Burst block, which is
        what makes is_combat_entity's blob grep work)
      _values: ValueTuple<uint, object[]>[] -- value vidx resolves to
        _values[vidx>>5].Item2[vidx&31] (stride 0x10, Item2 @+0x8, page 32)
    ZAttr objects: klass @+0x0 (Il2CppClass.name @+0x10), LongAttr value_ @+0x18,
    IntAttr value_ @+0x14.

Covers read_combat (structured Burst walk + layout cache), read_attr_map,
is_combat_entity, the read_hp HP-pair-invariant fallback, the implausible-value
rebuild path, and read_combat_batch's Python fallback.

History: the original version of this file (52e071a) synthesised only loose
LongAttr objects + raw ids sprinkled in a blob -- the layout of the pre-5fe3573
HP-invariant read_combat. After the structured Burst-index rewrite (5fe3573 /
7d3aa54) that fake no longer matched what read_combat walks, so 5 checks went
permanently red. This rewrite builds the real structure instead.

Run: python tools/mem_entity_combat_selftest.py
"""
from __future__ import annotations
import os, struct, sys
_HERE=os.path.dirname(os.path.abspath(__file__)); _ROOT=os.path.dirname(_HERE)
if _ROOT not in sys.path: sys.path.insert(0,_ROOT)
from mem_probe.il2cpp.mem_entity_combat import (
    EntityCombatReader, A_HP, A_MAX_HP, A_BREAK_STAGE, A_OVERDRIVE, A_STUN,
    A_EXT, A_MAX_EXT, A_SKILL_ID,
)


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

def _mk_attr_obj(m, klass, type_char, value):
    o=m.alloc(0x30); m.wu64(o, klass)
    if type_char=="L": m.w64(o+0x18, value)   # LongAttr.value_ @ +0x18
    else:              m.w32(o+0x14, value)   # IntAttr.value_  @ +0x14
    return o


class EntityBuilder:
    """Synthesise one entity with a real Burst index. Shares klass objects per
    FakeMem so the reader's klass-name cache behaves like live (same ptr reused)."""

    def __init__(self, m: FakeMem):
        self.m=m
        self.k_long=_mk_klass(m,"LongAttr")
        self.k_int=_mk_klass(m,"IntAttr")

    def build(self, attr_values, extra_longs=()):
        """attr_values: {attr_id: ('L'|'I', value)} -> ent_addr.

        The ZAttr objects for attr_values land in the paged _values object[]
        (multiple pages when >32, pinning the 0x10 tuple stride and vidx>>5
        paging); extra_longs are appended as additional LongAttr objects (no
        index entry) so read_hp's invariant scan sees them too.
        """
        m=self.m
        items=list(attr_values.items())
        n_objs=len(items)+len(extra_longs)
        n_pages=max(1,(n_objs+31)//32)
        # ---- _values: ValueTuple<uint, object[]>[] (stride 0x10, Item2 @ +0x8)
        vals=m.alloc(0x20+n_pages*0x10)
        pages=[]
        for p in range(n_pages):
            in_page=min(32, n_objs-p*32)
            arr=m.alloc(0x20+32*8); m.wu32(arr+0x18, in_page)
            m.wu64(vals+0x20+p*0x10+0x8, arr)            # _values[p].Item2
            pages.append(arr)
        def _place(vidx,o):
            m.wu64(pages[vidx>>5]+0x20+(vidx&31)*8, o)
        objs={}
        for vidx,(aid,(tc,val)) in enumerate(items):
            o=_mk_attr_obj(m, self.k_long if tc=="L" else self.k_int, tc, val)
            _place(vidx,o); objs[aid]=o
        for j,v in enumerate(extra_longs):
            _place(len(items)+j, _mk_attr_obj(m,self.k_long,"L",v))
        # ---- _indexPart: one Burst allocation; segments + vidx arrays INSIDE it
        ip=m.alloc(0x4000)
        per_seg=8
        for k in range((len(items)+per_seg-1)//per_seg):
            part=items[k*per_seg:(k+1)*per_seg]
            seg=ip+0x400+k*0x40                          # KeySegment inside the block
            vip=ip+0x2000+k*0x40                         # value-index array inside too
            for pos,(aid,_) in enumerate(part):
                m.wu32(seg+pos*4, aid)
                m.w32(vip+pos*4, k*per_seg+pos)          # vidx == insertion order
            m.w32(seg+0x20, len(part))                   # KeySegment.Count
            m.wu64(ip+0x10+k*8, seg)
            m.wu64(ip+0x110+k*8, vip)
        # ---- collection + entity
        attrs=m.alloc(0x40); m.wu64(attrs+0x18, ip); m.wu64(attrs+0x20, vals)
        ent=m.alloc(0x60); m.wu64(ent+0x48, attrs)
        return ent, objs


def main():
    print("test_entity_combat_reader")
    m=FakeMem()
    ecr=EntityCombatReader(m)
    eb=EntityBuilder(m)

    # dummy being hit: 10% HP + full state set; a server-time long sits in the
    # object[] as noise (excluded from read_hp by the plausibility cap)
    ent,objs=eb.build({
        A_HP:("L",177503), A_MAX_HP:("L",1775031),
        A_BREAK_STAGE:("I",2), A_OVERDRIVE:("I",7), A_STUN:("I",1),
        A_EXT:("I",33), A_MAX_EXT:("I",100), A_SKILL_ID:("I",9901),
    }, extra_longs=[12345, 1780837993397])
    check("is_combat_entity True (HP ids in _indexPart)", ecr.is_combat_entity(ent))
    c=ecr.read_combat(ent)
    check("read_combat returns dict", c is not None)
    check("max_hp = 1775031", c and c["max_hp"]==1775031)
    check("cur_hp = 177503", c and c["cur_hp"]==177503)
    check("hp_pct ~0.10", c and abs(c["hp_pct"]-0.1)<0.001)
    check("breaking_stage = 2", c and c["breaking_stage"]==2)
    check("overdrive/stun/ext/max_ext", c and (c["overdrive"],c["stun"],c["extinction"],c["max_extinction"])==(7,1,33,100))
    check("cast_skill_id = 9901", c and c["cast_skill_id"]==9901)

    # layout cache: mutate HP in place -> cached obj ptr must see the new value
    m.w64(objs[A_HP]+0x18, 888888)
    c=ecr.read_combat(ent)
    check("cached layout re-read sees mutated HP", c and c["cur_hp"]==888888)

    # implausible value -> rebuild path -> still rejected (returns None)
    m.w64(objs[A_HP]+0x18, 6_000_000_000_000)
    check("implausible HP -> None (rebuild path)", ecr.read_combat(ent) is None)
    m.w64(objs[A_HP]+0x18, 177503)
    check("recovers after value restored", (ecr.read_combat(ent) or {}).get("cur_hp")==177503)

    # full HP: both equal
    ent2,_=eb.build({A_HP:("L",1775031), A_MAX_HP:("L",1775031)})
    c2=ecr.read_combat(ent2)
    check("full HP -> 100%", c2 and c2["cur_hp"]==1775031 and c2["max_hp"]==1775031)
    check("missing state attrs -> None fields", c2 and c2["breaking_stage"] is None and c2["cast_skill_id"] is None)

    # read_attr_map: full structured {attr_id: value}, EXACT key set (no phantoms)
    amap=ecr.read_attr_map(ent)
    check("read_attr_map decodes all ids",
          amap.get(A_HP)==177503 and amap.get(A_MAX_HP)==1775031 and amap.get(A_BREAK_STAGE)==2)
    check("read_attr_map exact key set (extra_longs not keyed)",
          set(amap)=={A_HP,A_MAX_HP,A_BREAK_STAGE,A_OVERDRIVE,A_STUN,A_EXT,A_MAX_EXT,A_SKILL_ID})

    # read_hp HP-pair invariant fallback (legacy path, object[] scan)
    hp=ecr.read_hp(ent2)
    check("read_hp invariant fallback", hp==(1775031,1775031))
    # ent carries extra_longs incl. a server-time long (1.78e12): the plausibility
    # cap must exclude it, so max_hp is the real one -- pins MAX_HP_PLAUSIBLE
    check("read_hp excludes server-time long", ecr.read_hp(ent)==(177503,1775031))

    # read_combat_batch: FakeMem has no _handle -> cython path raises -> Python fallback
    batch=ecr.read_combat_batch([ent,ent2])
    check("read_combat_batch fallback parity",
          batch.get(ent,{}).get("cur_hp")==177503 and batch.get(ent2,{}).get("max_hp")==1775031)

    # multi-page _values + multi-segment index: 34 filler ints push the HP pair to
    # vidx 34/35 (page 1) across 5 KeySegments -- pins tuple stride 0x10, vidx>>5
    # paging, and the k*8 stride of both Burst tables
    big={9000+i:("I",i) for i in range(34)}
    big[A_HP]=("L",4200); big[A_MAX_HP]=("L",8400)
    ent4,_=eb.build(big)
    c4=ecr.read_combat(ent4)
    check("multi-page/multi-segment decode (vidx>32, k>0)",
          c4 and c4["cur_hp"]==4200 and c4["max_hp"]==8400)

    # HP acceptance predicate aligned with the Cython fast path:
    # server-time long as MaxHp -> reject; cur > max -> reject
    ent5,_=eb.build({A_HP:("L",100), A_MAX_HP:("L",1_780_837_993_397)})
    check("server-time MaxHp rejected (mx cap)", ecr.read_combat(ent5) is None)
    ent6,_=eb.build({A_HP:("L",200), A_MAX_HP:("L",100)})
    check("cur > max rejected (fast-path parity)", ecr.read_combat(ent6) is None)

    # non-combat entity (no HP ids in the index) -> None + cheap reject
    ent3,_=eb.build({A_BREAK_STAGE:("I",0)})
    check("non-combat entity -> read_combat None", ecr.read_combat(ent3) is None)
    check("non-combat entity -> is_combat_entity False", not ecr.is_combat_entity(ent3))

    # is_combat_entity blob-grep boundary: ids raw-planted in the LAST aligned
    # dword of the 0x4000 window must be seen (off-by-one regression guard)
    ent7,_=eb.build({A_BREAK_STAGE:("I",0)})
    attrs7=m.read_u64(ent7+0x48); ip7=m.read_u64(attrs7+0x18)
    m.wu32(ip7+0x3FF8, A_HP); m.wu32(ip7+0x3FFC, A_MAX_HP)
    check("is_combat_entity sees last aligned word of blob", ecr.is_combat_entity(ent7))

    print()
    if _fails: print(f"FAILED: {_fails}"); sys.exit(1)
    print("ALL PASS")


if __name__=="__main__":
    main()
