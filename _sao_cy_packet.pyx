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


cpdef tuple read_signed_varint(object data, Py_ssize_t pos):
    """Read a protobuf varint and return signed int32 plus new position."""
    cdef const unsigned char[:] src = data
    cdef object val_obj
    cdef Py_ssize_t new_pos
    val_obj, new_pos = _read_varint_u64(src, pos)
    return int(varint_to_int32(<unsigned long long>val_obj)), int(new_pos)


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


cpdef unsigned int read_le_u32_at(object data, Py_ssize_t pos):
    cdef const unsigned char[:] src = data
    if pos < 0 or pos + 4 > src.shape[0]:
        raise ValueError('read_le_u32_at out of range')
    return _read_le32(src, pos)


cpdef unsigned long long read_le_u64_at(object data, Py_ssize_t pos):
    cdef const unsigned char[:] src = data
    if pos < 0 or pos + 8 > src.shape[0]:
        raise ValueError('read_le_u64_at out of range')
    return _read_le64(src, pos)


cpdef object read_le_f32_at(object data, Py_ssize_t pos):
    cdef const unsigned char[:] src = data
    cdef U32F conv
    if pos < 0 or pos + 4 > src.shape[0]:
        raise ValueError('read_le_f32_at out of range')
    conv.u = _read_le32(src, pos)
    return float(conv.f)


cdef inline int _read_i32_padded(const unsigned char[:] src,
                                 Py_ssize_t* pos,
                                 Py_ssize_t length) except *:
    cdef Py_ssize_t offset = pos[0]
    cdef unsigned int u
    if offset + 8 > length:
        raise ValueError('unexpected eof while reading padded i32')
    u = _read_le32(src, offset)
    pos[0] = offset + 8
    if u >= 0x80000000u:
        return <int>(<long long>u - 0x100000000)
    return <int>u


cdef inline int _decimal_digits_ll(long long value) noexcept:
    cdef int digits = 1
    if value < 0:
        value = 0
    while value >= 10:
        value //= 10
        digits += 1
    return digits


cdef inline long long _append_decimal_ll(long long prefix,
                                         long long suffix,
                                         int min_width) noexcept:
    cdef int width
    cdef long long mul = 1
    cdef int i
    if prefix < 0:
        prefix = 0
    if suffix < 0:
        suffix = 0
    width = _decimal_digits_ll(suffix)
    if width < min_width:
        width = min_width
    for i in range(width):
        mul *= 10
    return prefix * mul + suffix


cpdef int raw_varint_to_int32(object raw):
    """Decode a raw varint payload to signed int32 with no Python fallback."""
    cdef const unsigned char[:] src = raw
    cdef Py_ssize_t pos = 0
    cdef Py_ssize_t length = src.shape[0]
    cdef unsigned long long val = 0
    cdef unsigned int shift = 0
    cdef unsigned char b
    cdef unsigned int val32
    while pos < length and pos < 10:
        b = src[pos]
        val |= (<unsigned long long>(b & 0x7F)) << shift
        pos += 1
        if (b & 0x80) == 0:
            break
        shift += 7
    val32 = <unsigned int>(val & 0xFFFFFFFFULL)
    if val32 >= 0x80000000u:
        return <int>(<long long>val32 - 0x100000000)
    return <int>val32


cpdef list decode_packed_varints(object raw):
    """Decode a packed varint byte payload into a Python list of ints."""
    cdef const unsigned char[:] src = raw
    cdef Py_ssize_t pos = 0
    cdef Py_ssize_t new_pos = 0
    cdef Py_ssize_t length = src.shape[0]
    cdef object val_obj
    cdef list values = []
    while pos < length:
        val_obj, new_pos = _read_varint_u64(src, pos)
        if new_pos <= pos:
            break
        values.append(int(val_obj))
        pos = new_pos
    return values


cpdef dict decode_resource_value_map(object resource_ids, object resources):
    """Decode paired resource id/value varints into {res_id: value}."""
    cdef dict result = {}
    cdef Py_ssize_t count
    cdef Py_ssize_t idx
    cdef object res_id_raw
    cdef object value_raw
    cdef int res_id
    cdef int value
    try:
        count = min(len(resource_ids or []), len(resources or []))
    except Exception:
        return result
    for idx in range(count):
        res_id_raw = resource_ids[idx]
        value_raw = resources[idx]
        if not isinstance(res_id_raw, int) or not isinstance(value_raw, int):
            continue
        res_id = varint_to_int32(<unsigned long long>res_id_raw)
        value = varint_to_int32(<unsigned long long>value_raw)
        if res_id > 0 and value >= 0:
            result[res_id] = value
    return result


cpdef long long append_decimal_key(object prefix, object suffix, int min_width):
    return _append_decimal_ll(<long long>int(prefix or 0),
                              <long long>int(suffix or 0),
                              min_width)


cpdef long long compute_damage_key(object owner_id, object damage_source,
                                   object owner_level, object hit_event_id):
    """Parser-side damage key composition without SkillFightLevelTable lookup."""
    cdef long long oid
    cdef long long source
    cdef long long hit
    cdef int damage_type
    try:
        oid = <long long>int(owner_id or 0)
    except Exception:
        return 0
    if oid <= 0:
        return 0
    try:
        source = <long long>int(damage_source or 0)
    except Exception:
        source = 0
    try:
        hit = <long long>int(hit_event_id or 0)
    except Exception:
        hit = 0
    if hit < 0:
        hit = 0
    if source == 2:
        damage_type = 2
    elif source > 0:
        damage_type = 3
    else:
        damage_type = 1
    return _append_decimal_ll(_append_decimal_ll(damage_type, oid, 0), hit, 2)


cpdef list parse_game_frame_headers(object frame):
    """Split concatenated game frames into (msg_type, is_zstd, payload)."""
    cdef const unsigned char[:] src = frame
    cdef Py_ssize_t total = src.shape[0]
    cdef Py_ssize_t offset = 0
    cdef unsigned int pkt_size
    cdef unsigned int pkt_type
    cdef list out = []
    if total < 6:
        return out
    while offset < total:
        if offset + 6 > total:
            break
        pkt_size = _read_be32(src, offset)
        if pkt_size < 6 or offset + <Py_ssize_t>pkt_size > total:
            break
        pkt_type = _read_be16(src, offset + 4)
        out.append((int(pkt_type & 0x7FFF), bool(pkt_type & 0x8000),
                    frame[offset + 6:offset + <Py_ssize_t>pkt_size]))
        offset += <Py_ssize_t>pkt_size
    return out


cpdef object parse_notify_header(object payload, unsigned long long expected_service_uuid):
    """Return (method_id, msg_payload) for c3SB Notify payloads, else None."""
    cdef const unsigned char[:] src = payload
    cdef Py_ssize_t length = src.shape[0]
    cdef unsigned long long service_uuid
    cdef unsigned int method_id
    if length < 16:
        return None
    service_uuid = (((<unsigned long long>src[0]) << 56)
                    | ((<unsigned long long>src[1]) << 48)
                    | ((<unsigned long long>src[2]) << 40)
                    | ((<unsigned long long>src[3]) << 32)
                    | ((<unsigned long long>src[4]) << 24)
                    | ((<unsigned long long>src[5]) << 16)
                    | ((<unsigned long long>src[6]) << 8)
                    | (<unsigned long long>src[7]))
    if service_uuid != expected_service_uuid:
        return None
    method_id = _read_be32(src, 12)
    return int(method_id), payload[16:]


cpdef tuple parse_dungeon_dirty_buffer(object data):
    """Parse SyncDungeonDirtyData's padded dirty buffer.

    Returns ``(flow_state_or_None, [{'target_id', 'nums', 'complete'}, ...])``.
    """
    cdef const unsigned char[:] src = data
    cdef Py_ssize_t length = src.shape[0]
    cdef Py_ssize_t pos = 0
    cdef Py_ssize_t root_end
    cdef Py_ssize_t flow_end
    cdef Py_ssize_t target_end
    cdef Py_ssize_t map_end
    cdef Py_ssize_t entry_end
    cdef int begin
    cdef int size
    cdef int field
    cdef int flow_field
    cdef int target_field
    cdef int map_field
    cdef int add_count
    cdef int remove_count
    cdef int update_count
    cdef int idx
    cdef int target_id
    cdef int nums
    cdef int complete
    cdef object flow_state = None
    cdef list targets = []

    begin = _read_i32_padded(src, &pos, length)
    if begin != -2:
        raise ValueError(f'invalid dirty container begin tag: {begin}')
    size = _read_i32_padded(src, &pos, length)
    if size == -3:
        return flow_state, targets
    if size < 0:
        raise ValueError(f'invalid dirty container size: {size}')
    root_end = pos + <Py_ssize_t>size
    if root_end > length:
        raise ValueError('dirty container body exceeds buffer size')

    field = _read_i32_padded(src, &pos, length)
    while field > 0 and pos <= length:
        if field == 2:
            begin = _read_i32_padded(src, &pos, length)
            if begin != -2:
                raise ValueError(f'invalid dirty container begin tag: {begin}')
            size = _read_i32_padded(src, &pos, length)
            if size == -3:
                pass
            elif size < 0:
                raise ValueError(f'invalid dirty container size: {size}')
            else:
                flow_end = pos + <Py_ssize_t>size
                if flow_end > length:
                    raise ValueError('dirty container body exceeds buffer size')
                flow_field = _read_i32_padded(src, &pos, length)
                while flow_field > 0:
                    if flow_field == 1:
                        flow_state = int(_read_i32_padded(src, &pos, length))
                    else:
                        pos = flow_end
                    if pos + 8 > length:
                        break
                    flow_field = _read_i32_padded(src, &pos, length)
                if flow_field != -3:
                    pos = flow_end
        elif field == 4:
            begin = _read_i32_padded(src, &pos, length)
            if begin != -2:
                raise ValueError(f'invalid dirty container begin tag: {begin}')
            size = _read_i32_padded(src, &pos, length)
            if size == -3:
                pass
            elif size < 0:
                raise ValueError(f'invalid dirty container size: {size}')
            else:
                target_end = pos + <Py_ssize_t>size
                if target_end > length:
                    raise ValueError('dirty container body exceeds buffer size')
                target_field = _read_i32_padded(src, &pos, length)
                while target_field > 0:
                    if target_field == 1:
                        add_count = _read_i32_padded(src, &pos, length)
                        remove_count = 0
                        update_count = 0
                        if add_count == -4:
                            pass
                        else:
                            if add_count == -1:
                                add_count = _read_i32_padded(src, &pos, length)
                            else:
                                remove_count = _read_i32_padded(src, &pos, length)
                                update_count = _read_i32_padded(src, &pos, length)
                            if add_count < 0 or remove_count < 0 or update_count < 0:
                                raise ValueError('negative dirty target map section size')
                            for idx in range(add_count):
                                _read_i32_padded(src, &pos, length)
                                target_id = 0
                                nums = 0
                                complete = 0
                                begin = _read_i32_padded(src, &pos, length)
                                if begin != -2:
                                    raise ValueError(f'invalid dirty container begin tag: {begin}')
                                size = _read_i32_padded(src, &pos, length)
                                if size == -3:
                                    pass
                                elif size < 0:
                                    raise ValueError(f'invalid dirty container size: {size}')
                                else:
                                    entry_end = pos + <Py_ssize_t>size
                                    if entry_end > length:
                                        raise ValueError('dirty container body exceeds buffer size')
                                    map_field = _read_i32_padded(src, &pos, length)
                                    while map_field > 0:
                                        if map_field == 1:
                                            target_id = _read_i32_padded(src, &pos, length)
                                        elif map_field == 2:
                                            nums = _read_i32_padded(src, &pos, length)
                                        elif map_field == 3:
                                            complete = _read_i32_padded(src, &pos, length)
                                        else:
                                            pos = entry_end
                                        if pos + 8 > length:
                                            break
                                        map_field = _read_i32_padded(src, &pos, length)
                                    if map_field != -3:
                                        pos = entry_end
                                targets.append({'target_id': int(target_id), 'nums': int(nums), 'complete': int(complete)})
                            for idx in range(remove_count):
                                _read_i32_padded(src, &pos, length)
                            for idx in range(update_count):
                                _read_i32_padded(src, &pos, length)
                                target_id = 0
                                nums = 0
                                complete = 0
                                begin = _read_i32_padded(src, &pos, length)
                                if begin != -2:
                                    raise ValueError(f'invalid dirty container begin tag: {begin}')
                                size = _read_i32_padded(src, &pos, length)
                                if size == -3:
                                    pass
                                elif size < 0:
                                    raise ValueError(f'invalid dirty container size: {size}')
                                else:
                                    entry_end = pos + <Py_ssize_t>size
                                    if entry_end > length:
                                        raise ValueError('dirty container body exceeds buffer size')
                                    map_field = _read_i32_padded(src, &pos, length)
                                    while map_field > 0:
                                        if map_field == 1:
                                            target_id = _read_i32_padded(src, &pos, length)
                                        elif map_field == 2:
                                            nums = _read_i32_padded(src, &pos, length)
                                        elif map_field == 3:
                                            complete = _read_i32_padded(src, &pos, length)
                                        else:
                                            pos = entry_end
                                        if pos + 8 > length:
                                            break
                                        map_field = _read_i32_padded(src, &pos, length)
                                    if map_field != -3:
                                        pos = entry_end
                                targets.append({'target_id': int(target_id), 'nums': int(nums), 'complete': int(complete)})
                    else:
                        pos = target_end
                    if pos + 8 > length:
                        break
                    target_field = _read_i32_padded(src, &pos, length)
                if target_field != -3:
                    pos = target_end
        else:
            pos = root_end
        if pos + 8 > length:
            break
        field = _read_i32_padded(src, &pos, length)
    if field != -3:
        pos = root_end
    return flow_state, targets


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
