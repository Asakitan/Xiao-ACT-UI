"""Proto-based entity locator — uses dump.cs klass RVAs to find Zproto.Entity
instances directly, then walks AttrCollection → Attr → ByteString → varint
to extract HP / MaxHP / break-bar / etc.

Layout (from dump.cs):
    Entity           +0x10 long Uuid    +0x18 EEntityType EntType
                     +0x20 AttrCollection*
    AttrCollection   +0x10 long Uuid    +0x18 RepeatedField<Attr>* Attrs
    RepeatedField<T> +0x10 T[] array    +0x18 int count
    Attr             +0x10 int Id       +0x18 ByteString*
    ByteString       +0x10 bool isBytesOwner   +0x18 ReadOnlyMemory<byte>
    ReadOnlyMemory   +0x18 byte[] obj   +0x20 int start   +0x24 int length
    byte[]           +0x20 raw data start

Entity klass RVA discovered values (script.json from regenerated dump):
    Zproto.Entity         RVA = 0x96C44D8  → klass_ptr = GA_base + 0x96C44D8
    Zproto.AttrCollection RVA = 0x96A3F08
    Zproto.Attr           RVA = 0x96A36D8
"""
from __future__ import annotations

import os
import struct
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as _cy

# Klass RVAs (relative to GameAssembly.dll base)
ENTITY_KLASS_RVA          = 0x96C44D8
ATTRCOLLECTION_KLASS_RVA  = 0x96A3F08
ATTR_KLASS_RVA            = 0x96A36D8
USERFIGHTATTR_KLASS_RVA   = 0x96A7C38
CHARSERIALIZE_KLASS_RVA   = 0x95F5E18

# Field offsets (from dump.cs)
ENTITY_UUID_OFF           = 0x10  # long
ENTITY_ENTTYPE_OFF        = 0x18  # EEntityType (i32)
ENTITY_ATTRS_OFF          = 0x20  # AttrCollection*

ATTRCOLL_UUID_OFF         = 0x10  # long
ATTRCOLL_ATTRS_OFF        = 0x18  # RepeatedField<Attr>*

# RepeatedField<T> stores `T[] array` and `int count`.
# Standard IL2CPP generic ref-type instance layout:
#   +0x00  klass_ptr
#   +0x08  monitor
#   +0x10  T[] array (ptr)
#   +0x18  int count
REPFIELD_ARRAY_OFF        = 0x10
REPFIELD_COUNT_OFF        = 0x18

# IL2CPP T[] array layout:
#   +0x00  klass_ptr
#   +0x08  monitor
#   +0x10  bounds
#   +0x18  max_length (i64)
#   +0x20  data start
ARRAY_DATA_OFF            = 0x20

ATTR_ID_OFF               = 0x10  # i32
ATTR_RAWDATA_OFF          = 0x18  # ByteString*

# ByteString stores the bytes via ReadOnlyMemory<byte>:
#   +0x10  bool isBytesOwner
#   +0x18  ReadOnlyMemory.object  (byte[] ref)
#   +0x20  ReadOnlyMemory.start   (i32)
#   +0x24  ReadOnlyMemory.length  (i32)
BYTESTR_OBJ_OFF           = 0x18
BYTESTR_START_OFF         = 0x20
BYTESTR_LENGTH_OFF        = 0x24

# AttrType IDs we care about (from packet_parser.AttrType)
ATTR_HP                   = 0x2C2E   # 11310 — current HP
ATTR_MAX_HP               = 0x2C38   # 11320 — max HP (rarely sent for monsters)
ATTR_NAME                 = 0x01
ATTR_TEMPLATE_ID          = 0x02     # placeholder, check packet_parser

# Monster-specific extended (from packet_parser.AttrType)
ATTR_MAX_EXTINCTION       = 440
ATTR_EXTINCTION           = 441
ATTR_BREAKING_STAGE       = 455
ATTR_MONSTER_SEASON_LEVEL = 462

ENT_TYPE_CHAR    = 0
ENT_TYPE_MONSTER = 1


def _decode_varint_i32(raw: bytes) -> int:
    """Decode a protobuf varint payload to signed int32. Pure-Python fallback
    when _sao_cy_packet isn't available; the real Cython kernel is faster but
    this discovery is one-shot so speed not critical here."""
    val = 0
    shift = 0
    for i, b in enumerate(raw):
        if i >= 10:
            break
        val |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            break
        shift += 7
    val32 = val & 0xFFFFFFFF
    if val32 >= 0x80000000:
        return val32 - 0x100000000
    return val32


@dataclass
class EntitySnapshot:
    addr: int            # Entity instance address
    uuid: int
    ent_type: int        # EEntityType
    attrs_coll_ptr: int  # AttrCollection*
    attrs: Dict[int, int] = field(default_factory=dict)  # attr_id -> decoded i32 value
    raw_attrs: List[Tuple[int, int]] = field(default_factory=list)  # [(attr_addr, attr_id), ...]


# ───────── GA module helpers ─────────

def _ga_base(pm) -> int:
    for m in pm.list_modules():
        if m.name.lower() == "gameassembly.dll":
            return m.base
    raise RuntimeError("GameAssembly.dll not found in modules")


def klass_ptr_for(pm, rva: int) -> int:
    return _ga_base(pm) + rva


# ───────── Klass instance scan ─────────

MAX_REGION_SIZE = 256 * 1024 * 1024


def find_klass_instances(pm, klass_ptr: int, *, n_workers: int = 8) -> List[int]:
    """Cython AVX2 full-heap scan for u64 == klass_ptr. Returns list of
    instance object addresses (each starts with the klass_ptr).
    """
    needle = int(klass_ptr) & 0xFFFFFFFFFFFFFFFF
    regions = [r for r in pm.iter_regions(only_readable=True, only_private=True)
               if r.size <= MAX_REGION_SIZE]
    out: List[int] = []
    lock = threading.Lock()

    def scan_one(region):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            return
        offs = _cy.find_aligned_u64(buf, needle)
        if not offs:
            return
        with lock:
            for off in offs:
                out.append(region.base + off)

    if n_workers > 1:
        with ThreadPoolExecutor(max_workers=n_workers) as ex:
            list(ex.map(scan_one, regions))
    else:
        for r in regions:
            scan_one(r)
    return out


# ───────── Entity field walking ─────────

def _read_repeated_field_attrs(pm, attrs_coll_ptr: int) -> List[int]:
    """Given AttrCollection*, return list of Attr* in its Attrs RepeatedField.
    Returns empty list on any read failure.
    """
    repfield_blob = pm.read_bytes(attrs_coll_ptr, 0x40)
    if not repfield_blob or len(repfield_blob) < ATTRCOLL_ATTRS_OFF + 8:
        return []
    repfield_ptr = int.from_bytes(
        repfield_blob[ATTRCOLL_ATTRS_OFF:ATTRCOLL_ATTRS_OFF + 8], "little")
    if not (0x10000 <= repfield_ptr <= 0x7FFFFFFFFFFF):
        return []
    rf_blob = pm.read_bytes(repfield_ptr, 0x40)
    if not rf_blob or len(rf_blob) < REPFIELD_COUNT_OFF + 4:
        return []
    array_ptr = int.from_bytes(
        rf_blob[REPFIELD_ARRAY_OFF:REPFIELD_ARRAY_OFF + 8], "little")
    count = int.from_bytes(
        rf_blob[REPFIELD_COUNT_OFF:REPFIELD_COUNT_OFF + 4], "little", signed=True)
    if count <= 0 or count > 1024 or not (0x10000 <= array_ptr <= 0x7FFFFFFFFFFF):
        return []
    # Read array elements in one RPM
    arr_total_size = ARRAY_DATA_OFF + count * 8
    arr_blob = pm.read_bytes(array_ptr, min(arr_total_size, 0x4000))
    if not arr_blob or len(arr_blob) < ARRAY_DATA_OFF:
        return []
    out: List[int] = []
    for i in range(count):
        off = ARRAY_DATA_OFF + i * 8
        if off + 8 > len(arr_blob):
            break
        ptr = int.from_bytes(arr_blob[off:off + 8], "little")
        if 0x10000 <= ptr <= 0x7FFFFFFFFFFF:
            out.append(ptr)
    return out


def _read_attr_id_and_rawbytes(pm, attr_ptr: int) -> Optional[Tuple[int, bytes]]:
    """Read Attr.{Id, RawData bytes}. Returns (id, raw_bytes) or None."""
    blob = pm.read_bytes(attr_ptr, ATTR_RAWDATA_OFF + 8)
    if not blob or len(blob) < ATTR_RAWDATA_OFF + 8:
        return None
    attr_id = int.from_bytes(blob[ATTR_ID_OFF:ATTR_ID_OFF + 4], "little", signed=True)
    bs_ptr = int.from_bytes(blob[ATTR_RAWDATA_OFF:ATTR_RAWDATA_OFF + 8], "little")
    if not (0x10000 <= bs_ptr <= 0x7FFFFFFFFFFF):
        return (attr_id, b"")
    bs_blob = pm.read_bytes(bs_ptr, BYTESTR_LENGTH_OFF + 4)
    if not bs_blob or len(bs_blob) < BYTESTR_LENGTH_OFF + 4:
        return (attr_id, b"")
    arr_ptr = int.from_bytes(bs_blob[BYTESTR_OBJ_OFF:BYTESTR_OBJ_OFF + 8], "little")
    start = int.from_bytes(bs_blob[BYTESTR_START_OFF:BYTESTR_START_OFF + 4], "little", signed=True)
    length = int.from_bytes(bs_blob[BYTESTR_LENGTH_OFF:BYTESTR_LENGTH_OFF + 4], "little", signed=True)
    if length < 0 or length > 256 or not (0x10000 <= arr_ptr <= 0x7FFFFFFFFFFF):
        return (attr_id, b"")
    # byte[] data starts at +0x20 of the array object
    raw = pm.read_bytes(arr_ptr + ARRAY_DATA_OFF + max(0, start), length)
    return (attr_id, raw or b"")


def read_entity_full(pm, entity_addr: int, *,
                     decode_attrs: bool = True,
                     attr_filter: Optional[set] = None
                     ) -> Optional[EntitySnapshot]:
    """Read a full Entity at entity_addr. Returns None if invalid."""
    body = pm.read_bytes(entity_addr, 0x40)
    if not body or len(body) < ENTITY_ATTRS_OFF + 8:
        return None
    uuid = int.from_bytes(body[ENTITY_UUID_OFF:ENTITY_UUID_OFF + 8], "little", signed=True)
    ent_type = int.from_bytes(body[ENTITY_ENTTYPE_OFF:ENTITY_ENTTYPE_OFF + 4], "little", signed=True)
    attrs_coll_ptr = int.from_bytes(body[ENTITY_ATTRS_OFF:ENTITY_ATTRS_OFF + 8], "little")
    snap = EntitySnapshot(addr=entity_addr, uuid=uuid, ent_type=ent_type,
                          attrs_coll_ptr=attrs_coll_ptr)
    if not decode_attrs or attrs_coll_ptr == 0:
        return snap
    if not (0x10000 <= attrs_coll_ptr <= 0x7FFFFFFFFFFF):
        return snap
    attr_ptrs = _read_repeated_field_attrs(pm, attrs_coll_ptr)
    for attr_ptr in attr_ptrs:
        result = _read_attr_id_and_rawbytes(pm, attr_ptr)
        if not result:
            continue
        attr_id, raw = result
        snap.raw_attrs.append((attr_ptr, attr_id))
        if attr_filter is not None and attr_id not in attr_filter:
            continue
        if not raw:
            continue
        # All known attrs we care about decode as varint i32
        snap.attrs[attr_id] = _decode_varint_i32(raw)
    return snap


def discover_all_entities(pm, *, ent_type_filter: Optional[int] = None,
                          decode_attrs: bool = True,
                          attr_filter: Optional[set] = None,
                          verbose: bool = True) -> List[EntitySnapshot]:
    """Find all live Zproto.Entity instances and decode their attributes."""
    t0 = time.time()
    klass_ptr = klass_ptr_for(pm, ENTITY_KLASS_RVA)
    if verbose:
        print(f"[proto-entity] Entity klass_ptr = 0x{klass_ptr:X}")
        print(f"[proto-entity] scanning Entity instances ...")
    inst_addrs = find_klass_instances(pm, klass_ptr)
    if verbose:
        print(f"[proto-entity] found {len(inst_addrs)} Entity instances "
              f"({time.time()-t0:.2f}s)")
    snapshots: List[EntitySnapshot] = []
    for addr in inst_addrs:
        snap = read_entity_full(pm, addr,
                                decode_attrs=decode_attrs,
                                attr_filter=attr_filter)
        if snap is None:
            continue
        if ent_type_filter is not None and snap.ent_type != ent_type_filter:
            continue
        # uuid==0 → pool-resident / cleared instance, skip
        if snap.uuid == 0:
            continue
        snapshots.append(snap)
    if verbose:
        print(f"[proto-entity] decoded {len(snapshots)} live entities "
              f"({time.time()-t0:.2f}s total)")
    return snapshots


__all__ = [
    "EntitySnapshot",
    "find_klass_instances", "klass_ptr_for",
    "read_entity_full", "discover_all_entities",
    "ENTITY_KLASS_RVA", "ATTRCOLLECTION_KLASS_RVA", "ATTR_KLASS_RVA",
    "ATTR_HP", "ATTR_MAX_HP", "ATTR_EXTINCTION", "ATTR_MAX_EXTINCTION",
    "ATTR_BREAKING_STAGE", "ENT_TYPE_CHAR", "ENT_TYPE_MONSTER",
]
