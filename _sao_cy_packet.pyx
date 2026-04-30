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


cpdef int decode_int32_from_raw(object raw):
    """Decode the project's raw int32 varint payload."""
    cdef const unsigned char[:] src = raw
    cdef object val_obj
    cdef Py_ssize_t pos
    cdef long long val
    if src.shape[0] <= 0:
        return 0
    val_obj, pos = _read_varint_u64(src, 0)
    val = <long long>val_obj
    if val > 0x7FFFFFFF:
        val -= 0x100000000
    return <int>val


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
