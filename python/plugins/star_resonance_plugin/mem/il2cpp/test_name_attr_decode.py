# -*- coding: utf-8 -*-
# Synthetic-memory test for EntityCombatReader.read_name_attr / _read_il2cpp_string.
#
# Builds a fake ZEntity -> ZAttrCollection -> Burst index -> _values -> ZAttr<string>
# -> Il2CppString layout in a flat dict-backed 'process memory' and asserts the NAME attr
# (id=1) decodes to the expected CN string. Verifies the index walk + UTF-16 string decode
# without the game running.  Run: python -m mem_probe.il2cpp.test_name_attr_decode
from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_combat import (
    EntityCombatReader, ENT_ATTRS_OFF, COLL_INDEXPART_OFF, COLL_VALUES_OFF,
    INDEX_KEYSEG_OFF, INDEX_VALIDX_OFF, KEYSEG_COUNT_OFF, ARRAY_ELEMS_OFF,
    VALUES_TUPLE_STRIDE, VALUES_TUPLE_ARR_OFF, ATTR_VAL8_OFF, STR_LEN_OFF, STR_CHARS_OFF,
)


class StubPM:
    def __init__(self):
        self.mem = {}

    def _wr(self, a, b):
        for i, byte in enumerate(b):
            self.mem[a + i] = byte

    def wu64(self, a, v):
        self._wr(a, int(v).to_bytes(8, "little"))

    def wu32(self, a, v):
        self._wr(a, (int(v) & 0xFFFFFFFF).to_bytes(4, "little"))

    def wbytes(self, a, b):
        self._wr(a, b)

    def read_bytes(self, a, n):
        return bytes(self.mem.get(a + i, 0) for i in range(n))

    def read_u64(self, a):
        return int.from_bytes(self.read_bytes(a, 8), "little")

    def read_u32(self, a):
        return int.from_bytes(self.read_bytes(a, 4), "little")

    def read_i32(self, a):
        v = self.read_u32(a)
        return v - 0x100000000 if v >= 0x80000000 else v

    def read_i64(self, a):
        v = self.read_u64(a)
        return v - 0x10000000000000000 if v >= 0x8000000000000000 else v


def _build(name: str):
    pm = StubPM()
    ent = 0x100000
    attrs = 0x200000
    ip = 0x300000
    vals = 0x400000
    segp = 0x310000
    vip = 0x320000
    arrp = 0x410000
    attr_obj = 0x420000
    strobj = 0x430000

    pm.wu64(ent + ENT_ATTRS_OFF, attrs)
    pm.wu64(attrs + COLL_INDEXPART_OFF, ip)
    pm.wu64(attrs + COLL_VALUES_OFF, vals)
    # KeySegment[0] -> segp ; valueIndices[0] -> vip
    pm.wu64(ip + INDEX_KEYSEG_OFF + 0 * 8, segp)
    pm.wu64(ip + INDEX_VALIDX_OFF + 0 * 8, vip)
    pm.wu32(segp + KEYSEG_COUNT_OFF, 1)        # one key in this segment
    pm.wu32(segp + 0 * 4, 1)                    # key[0] = attr_id 1 (NAME)
    pm.wu32(vip + 0 * 4, 0)                     # valueIndex[0] = 0
    # vidx 0 -> _values[0].Item2 object[] at arrp; element 0 -> the ZAttr object
    pm.wu64(vals + ARRAY_ELEMS_OFF + 0 * VALUES_TUPLE_STRIDE + VALUES_TUPLE_ARR_OFF, arrp)
    pm.wu64(arrp + ARRAY_ELEMS_OFF + 0 * 8, attr_obj)
    # ZAttr<string>.value_ (ref) @ +0x18 -> the Il2CppString
    pm.wu64(attr_obj + ATTR_VAL8_OFF, strobj)
    # Il2CppString: length @+0x10, UTF-16LE chars @+0x14
    chars = name.encode("utf-16-le")
    pm.wu32(strobj + STR_LEN_OFF, len(name))
    pm.wbytes(strobj + STR_CHARS_OFF, chars)
    return pm, ent


def main():
    failures = 0
    for nm in ("力之木桩", "Annana", "深渊领主·巴洛克"):
        pm, ent = _build(nm)
        got = EntityCombatReader(pm).read_name_attr(ent)
        ok = (got == nm)
        print(f"{'OK ' if ok else 'FAIL'}  expected={nm!r}  got={got!r}")
        failures += (0 if ok else 1)
    # absent NAME attr -> '' (empty index)
    pm = StubPM()
    pm.wu64(0x100000 + ENT_ATTRS_OFF, 0x200000)   # attrs but no index/values
    got = EntityCombatReader(pm).read_name_attr(0x100000)
    ok = (got == "")
    print(f"{'OK ' if ok else 'FAIL'}  absent-NAME -> {got!r}")
    failures += (0 if ok else 1)
    print("RESULT:", "ALL GREEN" if failures == 0 else f"{failures} FAILED")
    return failures


if __name__ == "__main__":
    raise SystemExit(1 if main() else 0)
