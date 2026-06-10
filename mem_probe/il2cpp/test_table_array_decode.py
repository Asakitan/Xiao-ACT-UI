# -*- coding: utf-8 -*-
"""Synthetic-memory test for MemConfigTableReader column decode.

Builds a fake ZLoader (IntArrayPool + Memory + _offsets dictionary) and row
blobs in a flat dict-backed 'process memory', then asserts:
  - col_i32_array decodes a pooled Int32Array column (i16 count + count*4 bytes)
  - the Pool length bit31 flag is masked before bound checks
  - empty arrays decode to []
  - out-of-range pool offsets / counts return [] instead of garbage
  - iter_rows_via_loader walks Dictionary<long,int> into (key, zloader, blob)

No game required.  Run: python -m mem_probe.il2cpp.test_table_array_decode
"""
from mem_probe.il2cpp.mem_config_table_reader import (
    MemConfigTableReader,
    ZLOADER_INTARRAYPOOL_OFF, ZLOADER_MEM_OBJ_OFF, ZLOADER_OFFSETS_OFF,
    ZLOADER_DATASIZE_OFF, ZLOADER_BUFRANGE_OFF,
    POOL_MEM_OBJ_OFF, POOL_MEM_IDX_OFF, POOL_MEM_LEN_OFF, POOL_DATASIZE_OFF,
    DICT_ENTRIES_OFF, DICT_COUNT_OFF, ENTRY_KEY_OFF, ENTRY_VAL_OFF, ENTRY_STRIDE,
    ARRAY_LEN_OFF, ARRAY_ELEMS_OFF,
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

    def wi16(self, a, v):
        self._wr(a, (int(v) & 0xFFFF).to_bytes(2, "little"))

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


class StubSR:
    def __init__(self, pm):
        self.pm = pm


class StubSource:
    def __init__(self, pm):
        self.sr = StubSR(pm)


ZL = 0x500000        # fake ZLoader object
POBJ = 0x600000      # IntArrayPool._memory backing byte[]
MEM = 0x700000       # ZLoader.Memory backing byte[] (row blobs)
POOL_IDX = 0x10
POOL_PAYLOAD = 0x200  # masked pool length
BLOB = MEM + 0x20 + 0x40   # one row blob inside Memory (mem idx 0, base_off 0x40)
ARR_COL = 0x18


def _build(*, len_flag_bit31=True):
    pm = StubPM()
    pool = ZL + ZLOADER_INTARRAYPOOL_OFF
    pm.wu64(pool + POOL_MEM_OBJ_OFF, POBJ)
    pm.wu32(pool + POOL_MEM_IDX_OFF, POOL_IDX)
    plen = POOL_PAYLOAD | (0x80000000 if len_flag_bit31 else 0)
    pm.wu32(pool + POOL_MEM_LEN_OFF, plen)
    pm.wu32(pool + POOL_DATASIZE_OFF, 4)
    # pooled Int32Array at pool offset 0x40: count=3, [111,222,333]
    base = POBJ + ARRAY_ELEMS_OFF + POOL_IDX + 0x40
    pm.wi16(base, 3)
    for i, v in enumerate((111, 222, 333)):
        pm.wu32(base + 2 + i * 4, v)
    # empty array at pool offset 0x60: count=0
    pm.wi16(POBJ + ARRAY_ELEMS_OFF + POOL_IDX + 0x60, 0)
    # overrun array at pool offset 0x1F0: count=100 -> 2+400 > payload
    pm.wi16(POBJ + ARRAY_ELEMS_OFF + POOL_IDX + 0x1F0, 100)
    # row blob columns
    pm.wu32(BLOB + 0x0, 9001)              # Id
    pm.wu32(BLOB + ARR_COL, 0x40)          # Int32Array column -> pool offset 0x40
    rd = MemConfigTableReader(StubSource(pm))
    return pm, rd


def _build_loader_walk(pm):
    """_offsets Dictionary<long,int> {9001: 1, 9002: 2} of RECORD indices.
    GetRowData semantics: blob = Memory + _bufferRange.offset + idx*DataSize."""
    pm.wu64(ZL + ZLOADER_MEM_OBJ_OFF, MEM)      # Memory._object
    pm.wu32(ZL + ZLOADER_MEM_OBJ_OFF + 8, 0)    # Memory._index
    pm.wu32(ZL + ZLOADER_DATASIZE_OFF, 0x40)    # 0x40-byte row records
    pm.wu32(ZL + ZLOADER_BUFRANGE_OFF, 0)       # _bufferRange.offset
    pm.wu32(ZL + ZLOADER_BUFRANGE_OFF + 4, 0x200)   # _bufferRange.len
    D, E = 0x900000, 0x910000
    pm.wu64(ZL + ZLOADER_OFFSETS_OFF, D)
    pm.wu32(D + DICT_COUNT_OFF, 3)
    pm.wu64(D + DICT_ENTRIES_OFF, E)
    pm.wu32(E + ARRAY_LEN_OFF, 3)
    for i, (k, v) in enumerate(((9001, 1), (9002, 2), (9099, 9))):
        ea = E + ARRAY_ELEMS_OFF + i * ENTRY_STRIDE
        pm.wu32(ea, 1)                          # hashCode >= 0 (occupied)
        pm.wu64(ea + ENTRY_KEY_OFF, k)
        pm.wu32(ea + ENTRY_VAL_OFF, v)
    pm.wu32(MEM + 0x20 + 1 * 0x40, 9001)        # blob col0 = Id
    pm.wu32(MEM + 0x20 + 2 * 0x40, 9002)
    # key 9099 -> record idx 9: 9*0x40+0x40 > _bufferRange.len -> must be skipped


def main():
    failures = 0

    def check(label, got, want):
        nonlocal failures
        ok = got == want
        print(f"{'OK ' if ok else 'FAIL'}  {label}: got={got!r} want={want!r}")
        failures += (0 if ok else 1)

    pm, rd = _build()
    check("decode [111,222,333] (len bit31 set)",
          rd.col_i32_array(ZL, BLOB, ARR_COL), [111, 222, 333])
    check("col_i32 Id", rd.col_i32(BLOB, 0), 9001)
    check("col_mlid alias", rd.col_mlid(BLOB, 0), 9001)

    pm2, rd2 = _build(len_flag_bit31=False)
    check("decode without bit31 flag",
          rd2.col_i32_array(ZL, BLOB, ARR_COL), [111, 222, 333])

    pm.wu32(BLOB + ARR_COL, 0x60)
    check("empty array (count=0)", rd.col_i32_array(ZL, BLOB, ARR_COL), [])

    pm.wu32(BLOB + ARR_COL, POOL_PAYLOAD + 8)
    check("pool offset out of bounds", rd.col_i32_array(ZL, BLOB, ARR_COL), [])

    pm.wu32(BLOB + ARR_COL, 0x1F0)
    check("count overruns pool", rd.col_i32_array(ZL, BLOB, ARR_COL), [])

    pm.wu32(BLOB + ARR_COL, 0xFFFFFFFF)         # i32 -1
    check("negative pool offset", rd.col_i32_array(ZL, BLOB, ARR_COL), [])

    check("None column", rd.col_i32_array(ZL, BLOB, None), [])
    check("zero blob col_i32", rd.col_i32(0, 0), None)

    _build_loader_walk(pm)
    rows = list(rd.iter_rows_via_loader("Whatever.TableBase", zloader=ZL))
    check("loader walk row count", len(rows), 2)
    if len(rows) == 2:
        check("loader walk keys", [k for k, _zl, _b in rows], [9001, 9002])
        check("loader walk blob col0 == key",
              [rd.col_i32(b, 0) for _k, _zl, b in rows], [9001, 9002])

    print("RESULT:", "ALL GREEN" if failures == 0 else f"{failures} FAILED")
    return failures


if __name__ == "__main__":
    raise SystemExit(1 if main() else 0)
