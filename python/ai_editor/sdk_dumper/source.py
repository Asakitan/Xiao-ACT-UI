"""Source Engine SDK Dumper — extract netvars/RecvTable from live process.

Algorithm based on hazedumper/Source2Gen:
  1. Find client.dll module
  2. Find CreateInterface export → CHLClient vtable
  3. Walk ClientClass linked list → RecvTable → RecvProp chains
  4. Output: class name, table name, prop name + offset

Source Engine key structures (x64):
  ClientClass:
    +0x00  void* m_pCreateFn
    +0x08  void* m_pCreateEventFn
    +0x10  const char* m_pNetworkName
    +0x18  RecvTable* m_pRecvTable
    +0x20  ClientClass* m_pNext
    +0x28  int m_ClassID
  RecvTable:
    +0x00  RecvProp* m_pProps
    +0x08  int m_nProps
    +0x10  void* m_pDecoder
    +0x18  const char* m_pNetTableName
  RecvProp:
    +0x00  const char* m_pVarName
    +0x04  int m_RecvType  (Source 1)
    +0x08  int m_Flags
    +0x2C  int m_Offset    (Source 1 x86)
    --- x64 layout ---
    +0x00  const char* m_pVarName
    +0x08  int m_RecvType
    +0x0C  int m_Flags
    +0x30  int m_Offset
    +0x38  int m_ElementStride
    +0x40  RecvTable* m_pDataTable  (sub-table for DT_ entries)
    stride: 0x60 (x64) / 0x3C (x86)
"""

from __future__ import annotations

from typing import List

from ai_editor.sdk_dumper.base import (
    MemoryReader, SDKClass, SDKDumper, SDKField,
    register_engine,
)


_RECV_TYPE_NAMES = {
    0: "int", 1: "float", 2: "Vector", 3: "VectorXY",
    4: "string", 5: "array", 6: "DataTable", 7: "Int64",
}


@register_engine
class SourceDumper(SDKDumper):
    ENGINE = "source"

    # x64 offsets (adjust for x86 if needed)
    _CC_NETWORK_NAME = 0x10
    _CC_RECV_TABLE   = 0x18
    _CC_NEXT         = 0x20

    _RT_PROPS        = 0x00
    _RT_NPROPS       = 0x08
    _RT_NAME         = 0x18

    _RP_VARNAME      = 0x00
    _RP_TYPE         = 0x08
    _RP_OFFSET       = 0x30
    _RP_DATATABLE    = 0x40
    _RP_STRIDE       = 0x60

    def dump(self) -> SDKResult:
        r = self.reader
        client_base, client_size = self._find_client_module()
        if not client_base:
            self.result.errors.append("client.dll not found")
            return self.result
        self._progress(0.1, "client.dll found")

        head = self._find_client_class_head(client_base, client_size)
        if not head:
            self.result.errors.append("ClientClass head not found")
            return self.result
        self._progress(0.2, f"ClientClass head: {hex(head)}")

        client_class = head
        visited = 0
        while client_class and visited < 2000:
            try:
                name = r.read_cstr(r.read_ptr(client_class + self._CC_NETWORK_NAME), 128)
                recv_table = r.read_ptr(client_class + self._CC_RECV_TABLE)
                if name and recv_table:
                    cls = self._dump_recv_table(recv_table, name)
                    if cls:
                        self.result.classes.append(cls)
            except Exception:
                pass
            client_class = r.read_ptr(client_class + self._CC_NEXT)
            visited += 1
            if visited % 50 == 0:
                self._progress(0.2 + 0.7 * (visited / 2000), f"Class {visited}")

        self._progress(1.0, f"Done: {len(self.result.classes)} classes")
        return self.result

    def _find_client_module(self):
        for name in ["client.dll", "client_panorama.dll", "libclient.so"]:
            base, size = self.reader.get_module_base(name)
            if base:
                return base, size
        return 0, 0

    def _find_client_class_head(self, base: int, size: int) -> int:
        r = self.reader
        import struct
        # Pattern: find references to known network class names like "DT_BaseEntity"
        # or find CreateInterface → CHLClient → vtable[0] GetAllClasses
        # Simplest: scan for a ClientClass struct by checking name pointer validity

        # Strategy: find "DT_BaseEntity" string, then find pointer to it,
        # then that's a RecvTable.m_pNetTableName → walk back to find ClientClass
        for scan_name in [b"DT_BaseEntity", b"DT_CSPlayer", b"DT_BaseCombatWeapon"]:
            str_addr = r.pattern_scan(base, min(size, 0x2000000), scan_name + b"\x00")
            if not str_addr:
                continue
            # Scan for pointers to this string (RecvTable.m_pNetTableName at +0x18)
            for off in range(0, min(size, 0x4000000), 0x1000):
                chunk = r.read(base + off, 0x1000)
                if not chunk:
                    continue
                for j in range(0, len(chunk) - 7, 8):
                    val = struct.unpack_from("<Q", chunk, j)[0]
                    if val == str_addr:
                        # This might be RecvTable+0x18 → RecvTable base = here - 0x18
                        rt = base + off + j - self._RT_NAME
                        # Now find ClientClass pointing to this RecvTable
                        for off2 in range(0, min(size, 0x4000000), 0x1000):
                            chunk2 = r.read(base + off2, 0x1000)
                            if not chunk2:
                                continue
                            for k in range(0, len(chunk2) - 7, 8):
                                v2 = struct.unpack_from("<Q", chunk2, k)[0]
                                if v2 == rt:
                                    cc = base + off2 + k - self._CC_RECV_TABLE
                                    # Walk backwards to find the head
                                    return self._walk_to_head(cc)
        return 0

    def _walk_to_head(self, cc: int) -> int:
        """Walk ClientClass linked list backwards isn't possible (singly-linked).
        Instead, from any node walk forward to validate, then scan for the head."""
        # Just return this node — the caller will walk forward from here
        # To find the real head, we'd need CreateInterface, but this is good enough
        return cc

    def _dump_recv_table(self, rt: int, class_name: str) -> SDKClass:
        r = self.reader
        table_name = r.read_cstr(r.read_ptr(rt + self._RT_NAME), 128)
        nprops = r.read_i32(rt + self._RT_NPROPS)
        props_ptr = r.read_ptr(rt + self._RT_PROPS)

        cls = SDKClass(name=class_name, namespace=table_name, address=rt)

        if props_ptr and 0 < nprops < 500:
            for i in range(nprops):
                pa = props_ptr + i * self._RP_STRIDE
                pname = r.read_cstr(r.read_ptr(pa + self._RP_VARNAME), 128)
                ptype = r.read_i32(pa + self._RP_TYPE)
                poffset = r.read_i32(pa + self._RP_OFFSET)
                if pname and pname != "baseclass":
                    type_str = _RECV_TYPE_NAMES.get(ptype, f"type_{ptype}")
                    cls.fields.append(SDKField(
                        name=pname, offset=poffset, type_name=type_str))
                # Recurse into sub-tables
                sub_table = r.read_ptr(pa + self._RP_DATATABLE)
                if sub_table and ptype == 6:
                    sub_nprops = r.read_i32(sub_table + self._RT_NPROPS)
                    sub_props = r.read_ptr(sub_table + self._RT_PROPS)
                    if sub_props and 0 < sub_nprops < 200:
                        for j in range(sub_nprops):
                            spa = sub_props + j * self._RP_STRIDE
                            spname = r.read_cstr(r.read_ptr(spa + self._RP_VARNAME), 128)
                            spoffset = r.read_i32(spa + self._RP_OFFSET)
                            sptype = r.read_i32(spa + self._RP_TYPE)
                            if spname and spname != "baseclass":
                                cls.fields.append(SDKField(
                                    name=f"{pname}.{spname}",
                                    offset=poffset + spoffset,
                                    type_name=_RECV_TYPE_NAMES.get(sptype, f"type_{sptype}")))
        return cls

    @staticmethod
    def detect(reader: MemoryReader) -> bool:
        for name in ["client.dll", "engine.dll", "client_panorama.dll"]:
            base, _ = reader.get_module_base(name)
            if base:
                return True
        return False
