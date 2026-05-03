# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Mandatory Cython byte-level helpers for packet parsing and capture."""


cdef union U32F:
    unsigned int u
    float f


cdef inline unsigned int _read_be16(const unsigned char[:] src,
                                    Py_ssize_t pos) nogil:
    return ((<unsigned int>src[pos]) << 8) | (<unsigned int>src[pos + 1])


cdef inline unsigned int _read_be32(const unsigned char[:] src,
                                    Py_ssize_t pos) nogil:
    return (((<unsigned int>src[pos]) << 24)
            | ((<unsigned int>src[pos + 1]) << 16)
            | ((<unsigned int>src[pos + 2]) << 8)
            | (<unsigned int>src[pos + 3]))


cdef inline unsigned int _read_le32(const unsigned char[:] src,
                                    Py_ssize_t pos) nogil:
    return ((<unsigned int>src[pos])
            | ((<unsigned int>src[pos + 1]) << 8)
            | ((<unsigned int>src[pos + 2]) << 16)
            | ((<unsigned int>src[pos + 3]) << 24))


cdef inline unsigned long long _read_le64(const unsigned char[:] src,
                                          Py_ssize_t pos) nogil:
    return ((<unsigned long long>src[pos])
            | ((<unsigned long long>src[pos + 1]) << 8)
            | ((<unsigned long long>src[pos + 2]) << 16)
            | ((<unsigned long long>src[pos + 3]) << 24)
            | ((<unsigned long long>src[pos + 4]) << 32)
            | ((<unsigned long long>src[pos + 5]) << 40)
            | ((<unsigned long long>src[pos + 6]) << 48)
            | ((<unsigned long long>src[pos + 7]) << 56))


cdef inline tuple _read_varint_u64(const unsigned char[:] src,
                                   Py_ssize_t pos):
    cdef unsigned long long result = 0
    cdef unsigned int shift = 0
    cdef unsigned char b
    cdef Py_ssize_t length = src.shape[0]
    if pos < 0:
        pos = 0
    while pos < length:
        b = src[pos]
        pos += 1
        if shift < 64:
            result |= (<unsigned long long>(b & 0x7F)) << shift
        if (b & 0x80) == 0:
            return int(result), int(pos)
        shift += 7
    return int(result), int(pos)


cpdef tuple read_varint(object data, Py_ssize_t pos):
    """Read a protobuf varint and return ``(value, new_pos)``."""
    cdef const unsigned char[:] src = data
    return _read_varint_u64(src, pos)


cpdef long long varint_to_int64(unsigned long long val):
    """Two's-complement reinterpretation of a 64-bit varint."""
    if val > 0x7FFFFFFFFFFFFFFFULL:
        return <long long>(val - (1ULL << 63)) - (1LL << 63)
    return <long long>val


cpdef int varint_to_int32(unsigned long long val):
    """Two's-complement reinterpretation of a 32-bit varint payload."""
    cdef unsigned int u = <unsigned int>(val & 0xFFFFFFFFULL)
    if u > 0x7FFFFFFFu:
        return <int>(u - 0x80000000u) - 0x80000000
    return <int>u


cpdef str decode_string_from_raw(object raw):
    """Match ``protobufjs reader.string()``: ``[varint length][utf-8 bytes]``.

    Falls back to decoding the entire payload as utf-8 when the leading
    varint length is invalid or absent (matches the legacy Python helper).
    """
    if raw is None:
        return ''
    cdef const unsigned char[:] src = raw
    cdef Py_ssize_t length = src.shape[0]
    if length <= 0:
        return ''
    cdef object str_len_obj
    cdef Py_ssize_t pos = 0
    cdef Py_ssize_t end
    cdef Py_ssize_t str_len
    try:
        str_len_obj, pos = _read_varint_u64(src, 0)
        str_len = <Py_ssize_t>int(str_len_obj)
        end = pos + str_len
        if str_len > 0 and end <= length:
            return bytes(raw[pos:end]).decode('utf-8', 'ignore')
    except Exception:
        pass
    try:
        return bytes(raw).decode('utf-8', 'ignore')
    except Exception:
        return ''


cpdef bint is_sane_attr_stamina_max(object value):
    """STA cap acceptance heuristic from packet_parser.

    Mirrors `_is_sane_attr_stamina_max`: ``0 < value <= 1300``.
    """
    cdef long long v
    try:
        v = <long long>int(value or 0)
    except Exception:
        return False
    return v > 0 and v <= 1300


cpdef object decode_dirty_energy_value(object raw_u32, object raw_f32,
                                       object stamina_max=None):
    """Pick the sane stamina value out of a dirty-stream pair (u32/f32).

    Matches `_decode_dirty_energy_value`: prefer f32 when finite and sensible,
    otherwise the u32 if within the cap. Returns ``None`` on rejection.
    """
    cdef double f32
    cdef long long u32
    cdef long long sta_max_int = 0
    cdef double max_allowed
    try:
        u32 = <long long>int(raw_u32 or 0)
    except Exception:
        u32 = 0
    try:
        f32 = <double>float(raw_f32 if raw_f32 is not None else 0.0)
    except Exception:
        f32 = 0.0
    if stamina_max is not None:
        try:
            sta_max_int = <long long>int(stamina_max or 0)
        except Exception:
            sta_max_int = 0
    if sta_max_int > 0:
        max_allowed = <double>sta_max_int * 1.2
        if max_allowed < 20000.0:
            max_allowed = 20000.0
    else:
        max_allowed = 20000.0

    # math.isfinite check inlined for f32.
    cdef bint finite = not (f32 != f32 or f32 > 1e308 or f32 < -1e308)
    if finite:
        if 0.0 <= f32 <= 1.05 and sta_max_int > 0:
            return float(f32)
        if 0.01 <= f32 <= max_allowed:
            return float(f32)
        if f32 == 0.0:
            return 0.0
    if 0 <= u32 <= <long long>max_allowed:
        return float(u32)
    return None


cpdef int normalize_season_medal_level(object raw_level):
    """Clamp a season-medal level to ``>= 0`` ints (mirrors Python helper)."""
    cdef long long v
    try:
        v = <long long>int(raw_level or 0)
    except Exception:
        return 0
    if v < 0:
        return 0
    return <int>v


# ── level_extra source priority (fixed table, queried per dirty packet) ──
_LEVEL_EXTRA_SOURCE_PRIORITY = {
    'deep_sleep': 200,
    'season_attr': 100,
    'season_attr_lv': 100,
    'season_medal': 50,
    'monster_hunt': 10,
    'battlepass': 5,
    'battlepass_data': 3,
}


cpdef int level_extra_source_priority(object source):
    """Lookup priority for a level_extra source name (0 when unknown)."""
    if source is None:
        return 0
    cdef str s = str(source) if not isinstance(source, str) else source
    return <int>_LEVEL_EXTRA_SOURCE_PRIORITY.get(s, 0)


cpdef bint attrs_match_monster_hint(list attr_ids, frozenset hint_set):
    """Return True when any element of ``attr_ids`` is in ``hint_set``.

    Used by `_attrs_look_monster_like` after Python collects the attr ids
    from a pb2 ``AttrCollection``. The membership test is a single C-level
    set lookup per id.
    """
    if not attr_ids or not hint_set:
        return False
    cdef object x
    for x in attr_ids:
        if x in hint_set:
            return True
    return False


cpdef int decode_int32_from_raw(object raw):
    """Decode the project's raw int32 varint payload."""
    cdef const unsigned char[:] src = raw
    cdef object val_obj
    cdef Py_ssize_t pos
    cdef unsigned int val32
    if src.shape[0] <= 0:
        return 0
    val_obj, pos = _read_varint_u64(src, 0)
    val32 = <unsigned int>(val_obj & 0xFFFFFFFF)
    if val32 >= 0x80000000:
        return <int>(<long long>val32 - 0x100000000)
    return <int>val32


cpdef object decode_float32_from_raw(object raw):
    """Decode a little-endian float32 payload, or None for short data."""
    cdef const unsigned char[:] src = raw
    cdef U32F conv
    if src.shape[0] < 4:
        return None
    conv.u = _read_le32(src, 0)
    return float(conv.f)


cpdef dict decode_fields(object data):
    """Decode the supported protobuf wire types into ``{field: [values]}``."""
    cdef const unsigned char[:] src = data
    cdef dict fields = {}
    cdef object lst
    cdef object tag_obj
    cdef object val_obj
    cdef unsigned long long tag
    cdef unsigned long long vlen
    cdef unsigned int field_num
    cdef unsigned int wire_type
    cdef unsigned long long u64
    cdef Py_ssize_t pos = 0
    cdef Py_ssize_t new_pos = 0
    cdef Py_ssize_t length = src.shape[0]
    cdef U32F f32

    while pos < length:
        tag_obj, new_pos = _read_varint_u64(src, pos)
        tag = <unsigned long long>tag_obj
        pos = new_pos
        field_num = <unsigned int>(tag >> 3)
        wire_type = <unsigned int>(tag & 0x07)

        if wire_type == 0:
            val_obj, new_pos = _read_varint_u64(src, pos)
            pos = new_pos
        elif wire_type == 1:
            if pos + 8 > length:
                break
            u64 = _read_le64(src, pos)
            pos += 8
            if u64 >= 0x8000000000000000:
                val_obj = int(u64) - (1 << 64)
            else:
                val_obj = int(u64)
        elif wire_type == 2:
            val_obj, new_pos = _read_varint_u64(src, pos)
            vlen = <unsigned long long>val_obj
            pos = new_pos
            if vlen > <unsigned long long>(length - pos):
                break
            val_obj = data[pos:pos + <Py_ssize_t>vlen]
            pos += <Py_ssize_t>vlen
        elif wire_type == 5:
            if pos + 4 > length:
                break
            f32.u = _read_le32(src, pos)
            pos += 4
            val_obj = float(f32.f)
        else:
            break

        lst = fields.get(field_num)
        if lst is None:
            lst = []
            fields[field_num] = lst
        lst.append(val_obj)

    return fields


cpdef bint scan_c3sb_nested(object data):
    """Scan nested game frames for the c3SB signature used by server detect."""
    cdef const unsigned char[:] src = data
    cdef Py_ssize_t offset = 0
    cdef Py_ssize_t length = src.shape[0]
    cdef unsigned int plen
    cdef Py_ssize_t end
    cdef Py_ssize_t payload_start

    while offset + 4 < length:
        plen = _read_be32(src, offset)
        if plen < 6 or plen > 0xFFFFF:
            break
        end = offset + <Py_ssize_t>plen
        if end > length:
            break
        payload_start = offset + 4
        if end - payload_start > 11:
            if (src[payload_start + 5] == 0
                    and src[payload_start + 6] == 0x63
                    and src[payload_start + 7] == 0x33
                    and src[payload_start + 8] == 0x53
                    and src[payload_start + 9] == 0x42
                    and src[payload_start + 10] == 0):
                return True
        offset = end
    return False


cpdef int find_frame_realign(object data, Py_ssize_t max_scan=65536):
    """Return next valid game-frame offset, or -1 when none is found."""
    cdef const unsigned char[:] src = data
    cdef Py_ssize_t length = src.shape[0]
    cdef Py_ssize_t scan_end = length - 5
    cdef Py_ssize_t i
    cdef unsigned int sz
    cdef unsigned int tp
    cdef unsigned int msg

    if max_scan > 0 and scan_end > max_scan:
        scan_end = max_scan
    if scan_end <= 1:
        return -1
    with nogil:
        for i in range(1, scan_end):
            sz = _read_be32(src, i)
            if 6 <= sz <= 0x0FFFFF:
                tp = _read_be16(src, i + 4)
                msg = tp & 0x7FFF
                if msg == 2 or msg == 3 or msg == 4 or msg == 5 or msg == 6:
                    return <int>i
    return -1


cpdef object parse_eth_ip_tcp(object raw):
    """Parse Ethernet -> IPv4 -> TCP frame headers.

    Returns ``(src_ip, dst_ip, sport, dport, seq, payload, ip_id, frag_offset,
    more_frag)`` or ``None``. This mirrors packet_capture._parse_eth_ip_tcp's
    historical semantics, including returning IPv4 fragment payload for
    non-TCP packets so the Python reassembler can cache fragments.
    """
    cdef const unsigned char[:] src = raw
    cdef Py_ssize_t length = src.shape[0]
    cdef Py_ssize_t ip_off = 14
    cdef Py_ssize_t tcp_off
    cdef Py_ssize_t payload_off
    cdef unsigned int eth_type
    cdef unsigned int ver_ihl
    cdef unsigned int ihl
    cdef unsigned int total_len
    cdef unsigned int ip_id
    cdef unsigned int flags_frag
    cdef unsigned int proto
    cdef unsigned int sport
    cdef unsigned int dport
    cdef unsigned int seq
    cdef unsigned int data_offset
    cdef bint more_frag
    cdef unsigned int frag_offset
    cdef Py_ssize_t end

    if length < 54:
        return None
    eth_type = _read_be16(src, 12)
    if eth_type != 0x0800:
        return None

    ver_ihl = src[ip_off]
    ihl = (ver_ihl & 0x0F) * 4
    if ihl < 20 or ip_off + <Py_ssize_t>ihl > length:
        return None

    total_len = _read_be16(src, ip_off + 2)
    ip_id = _read_be16(src, ip_off + 4)
    flags_frag = _read_be16(src, ip_off + 6)
    more_frag = (flags_frag & 0x2000) != 0
    frag_offset = flags_frag & 0x1FFF
    proto = src[ip_off + 9]
    end = ip_off + <Py_ssize_t>total_len

    if proto != 6:
        return (
            raw[ip_off + 12:ip_off + 16],
            raw[ip_off + 16:ip_off + 20],
            0,
            0,
            0,
            raw[ip_off + <Py_ssize_t>ihl:end],
            int(ip_id),
            int(frag_offset),
            bool(more_frag),
        )

    tcp_off = ip_off + <Py_ssize_t>ihl
    if tcp_off + 20 > length:
        return None
    sport = _read_be16(src, tcp_off)
    dport = _read_be16(src, tcp_off + 2)
    seq = _read_be32(src, tcp_off + 4)
    data_offset = ((src[tcp_off + 12] >> 4) & 0x0F) * 4
    payload_off = tcp_off + <Py_ssize_t>data_offset
    if payload_off < length:
        return (
            raw[ip_off + 12:ip_off + 16],
            raw[ip_off + 16:ip_off + 20],
            int(sport),
            int(dport),
            int(seq),
            raw[payload_off:end],
            int(ip_id),
            int(frag_offset),
            bool(more_frag),
        )
    return (
        raw[ip_off + 12:ip_off + 16],
        raw[ip_off + 16:ip_off + 20],
        int(sport),
        int(dport),
        int(seq),
        b'',
        int(ip_id),
        int(frag_offset),
        bool(more_frag),
    )
