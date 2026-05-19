# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Mandatory Cython byte-level helpers for packet parsing and capture."""


cdef union U32F:
    unsigned int u
    float f


cdef inline long long _safe_i64(object value, long long default=0):
    if value is None:
        return default
    try:
        return <long long>int(value)
    except Exception:
        return default


cdef inline double _safe_f64(object value, double default=0.0):
    if value is None:
        return default
    try:
        return <double>float(value)
    except Exception:
        return default


cdef inline int _round_pos_i32(double value):
    if value <= 0.0:
        return 0
    return <int>(value + 0.5)


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


cpdef int sanitize_packet_stamina_max(object candidate_obj, object previous_obj):
    """Reject implausible stamina-max spikes before they reach the HUD."""
    cdef int candidate = <int>_safe_i64(candidate_obj, 0)
    cdef int previous = <int>_safe_i64(previous_obj, 0)
    cdef int grow_tol
    cdef int shrink_tol
    if candidate <= 0:
        return previous if previous > 0 else 0
    if candidate > 1300:
        if 0 < previous <= 1300:
            return previous
        return 0
    if 0 < previous <= 200 and 500 <= candidate <= 1300:
        return candidate
    if 0 < previous <= 1300:
        grow_tol = <int>(previous * 0.10)
        if grow_tol < 80:
            grow_tol = 80
        if candidate > previous + grow_tol:
            return previous
        shrink_tol = <int>(previous * 0.35)
        if shrink_tol < 220:
            shrink_tol = 220
        if candidate < previous - shrink_tol:
            return previous
    return candidate


cpdef object resolve_packet_stamina(object energy_value_obj, object stamina_max_obj):
    """Convert packet energy value into ``(current, pct)`` when sane."""
    cdef int stamina_max = <int>_safe_i64(stamina_max_obj, 0)
    cdef double value
    cdef int current
    if stamina_max <= 0:
        return None
    try:
        value = float(energy_value_obj)
    except Exception:
        return None
    if value != value or value < 0.0 or value > 1e308:
        return None
    if value <= 1.05 and stamina_max > 1:
        current = _round_pos_i32(stamina_max * value)
    else:
        current = _round_pos_i32(value)
    if current < 0:
        return None
    if current > stamina_max:
        if current <= <int>(stamina_max * 1.2):
            current = stamina_max
        else:
            return None
    return (current, (<double>current / <double>stamina_max) if stamina_max > 0 else 0.0)


cpdef object resolve_resource_stamina(object resource_id_obj,
                                      object resource_values,
                                      object energy_info_map,
                                      object stamina_max_obj):
    """Convert a resolved stamina resource entry into ``(current, pct)``."""
    cdef int resource_id = <int>_safe_i64(resource_id_obj, 0)
    cdef int stamina_max = <int>_safe_i64(stamina_max_obj, 0)
    cdef int current
    cdef object energy_info
    cdef object get_fn
    cdef int resource_max
    if resource_id <= 0 or resource_values is None:
        return None
    if resource_id not in resource_values:
        return None
    current = <int>_safe_i64(resource_values.get(resource_id, 0), 0)
    if current < 0:
        return None
    energy_info = (energy_info_map or {}).get(resource_id) or {}
    get_fn = energy_info.get if energy_info is not None and hasattr(energy_info, 'get') else None
    if get_fn is not None:
        resource_max = <int>_safe_i64(get_fn('energy_value', 0), 0)
    else:
        resource_max = 0
    if resource_max > 0:
        if stamina_max <= 0:
            stamina_max = resource_max
        else:
            stamina_max = min(max(stamina_max, current), resource_max)
    if stamina_max <= 0:
        return None
    if current > stamina_max:
        if current <= <int>(stamina_max * 1.15):
            current = stamina_max
        else:
            return None
    return (current, (<double>current / <double>stamina_max) if stamina_max > 0 else 0.0)


cpdef object resolve_ratio_stamina(object ratio_obj, object stamina_max_obj):
    """Convert a 0..1 ratio into ``(current, pct)`` when sane."""
    cdef int stamina_max = <int>_safe_i64(stamina_max_obj, 0)
    cdef double ratio
    cdef int current
    if stamina_max <= 0:
        return None
    try:
        ratio = float(ratio_obj)
    except Exception:
        return None
    if ratio != ratio:
        return None
    if ratio < 0.0:
        ratio = 0.0
    elif ratio > 1.0:
        ratio = 1.0
    current = _round_pos_i32(stamina_max * ratio)
    return (current, ratio)


cpdef int get_skill_id_for_level(object skill_level_info_map,
                                 object skill_level_id_obj):
    """Resolve skill_id from the skill-level info map with a numeric fallback."""
    cdef int skill_level_id = <int>_safe_i64(skill_level_id_obj, 0)
    cdef object skill_info
    cdef object get_fn
    cdef int skill_id
    if skill_level_id <= 0:
        return 0
    skill_info = (skill_level_info_map or {}).get(skill_level_id) or {}
    get_fn = skill_info.get if skill_info is not None and hasattr(skill_info, 'get') else None
    if get_fn is not None:
        skill_id = <int>_safe_i64(get_fn('skill_id', 0), 0)
    else:
        skill_id = 0
    if skill_id > 0:
        return skill_id
    if skill_level_id >= 100:
        return <int>(skill_level_id // 100)
    return skill_level_id


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


cpdef int pick_stamina_resource_id(object resource_values,
                                   object energy_info_map,
                                   object total_limit_obj):
    """Pick the most plausible stamina resource id for a player."""
    cdef object rid_obj
    cdef object cur_obj
    cdef object info
    cdef object get_fn
    cdef long long total_limit = _safe_i64(total_limit_obj, 0)
    cdef long long rid
    cdef long long current_value
    cdef long long max_value
    cdef long long unlock_num
    cdef int score
    cdef int tie_score
    cdef int best_score = -2147483648
    cdef int best_tie = -2147483648
    cdef int best_rid = 0
    cdef bint found = False
    cdef long long tol
    if resource_values is None:
        resource_values = {}
    if energy_info_map is None:
        energy_info_map = {}

    for rid_obj, cur_obj in resource_values.items():
        rid = _safe_i64(rid_obj, 0)
        if rid <= 0:
            continue
        current_value = _safe_i64(cur_obj, 0)
        info = energy_info_map.get(rid_obj) or energy_info_map.get(<int>rid) or {}
        get_fn = info.get if hasattr(info, 'get') else None
        if get_fn is not None:
            max_value = _safe_i64(get_fn('energy_value', 0), 0)
            unlock_num = _safe_i64(get_fn('unlock_num', 0), 0)
        else:
            max_value = 0
            unlock_num = 0
        if max_value < 0:
            max_value = 0
        score = 0
        tie_score = 0
        if max_value > 0:
            score += 50
            if 0 <= current_value <= max_value:
                score += 25
            if total_limit > 0:
                tol = <long long>(total_limit * 0.08)
                if tol < 12:
                    tol = 12
                if total_limit >= max_value:
                    if total_limit - max_value <= tol:
                        score += 25
                elif max_value - total_limit <= tol:
                    score += 25
                tie_score = -<int>abs(total_limit - max_value)
            if is_sane_attr_stamina_max(max_value):
                score += 15
        if 0 <= current_value <= 1300:
            score += 8
        if unlock_num >= 0:
            score += 2
        if (not found or score > best_score
                or (score == best_score and tie_score > best_tie)):
            best_score = score
            best_tie = tie_score
            best_rid = <int>rid
            found = True

    if found:
        return best_rid
    if len(energy_info_map) == 1:
        try:
            return <int>_safe_i64(next(iter(energy_info_map.keys())), 0)
        except Exception:
            return 0
    return 0


cpdef bint burst_ready(object skill_slots, object watched_slots):
    """Return True when every watched slot is ready and at least one matched."""
    cdef set watched = set()
    cdef object raw
    cdef object slot
    cdef long long idx
    cdef str state
    cdef long long charge_count
    cdef long long remaining_ms
    cdef object cooldown_pct
    cdef bint matched = False
    if not skill_slots or not watched_slots:
        return False
    try:
        for raw in watched_slots:
            idx = _safe_i64(raw, 0)
            if idx > 0:
                watched.add(<int>idx)
    except Exception:
        watched = set()
    if not watched:
        return False

    for slot in skill_slots:
        if not isinstance(slot, dict):
            continue
        idx = _safe_i64(slot.get('index', 0), 0)
        if idx not in watched:
            continue
        matched = True
        state = str(slot.get('state', '') or '').strip().lower()
        if state in ('ready', 'active'):
            continue
        try:
            if bool(slot.get('active')):
                continue
        except Exception:
            pass
        charge_count = _safe_i64(slot.get('charge_count', 0), 0)
        if charge_count > 0:
            continue
        remaining_ms = _safe_i64(slot.get('remaining_ms', 0), 0)
        if remaining_ms <= 120:
            continue
        cooldown_pct = slot.get('cooldown_pct', 1.0)
        try:
            if float(cooldown_pct) <= 0.02:
                continue
        except Exception:
            pass
        return False
    return matched


cpdef bint slot_ready(object slot):
    """Return True when a skill slot is effectively ready."""
    cdef str state
    cdef long long charge_count
    cdef long long remaining_ms
    cdef object cooldown_pct
    if not isinstance(slot, dict):
        return False
    state = str(slot.get('state', '') or '').strip().lower()
    if state in ('ready', 'active'):
        return True
    try:
        if bool(slot.get('active')):
            return True
    except Exception:
        pass
    charge_count = _safe_i64(slot.get('charge_count', 0), 0)
    if charge_count > 0:
        return True
    remaining_ms = _safe_i64(slot.get('remaining_ms', 0), 0)
    if remaining_ms <= 120:
        return True
    cooldown_pct = slot.get('cooldown_pct', 1.0)
    try:
        return float(cooldown_pct) <= 0.02
    except Exception:
        return False


cpdef int apply_ready_edges(object skill_slots, object previous_slots):
    """Mutate current slots in-place with ready_edge flags. Returns number of edges."""
    cdef object slot
    cdef object prev_slot
    cdef long long idx
    cdef int edges = 0
    if not skill_slots:
        return 0
    for slot in skill_slots:
        if not isinstance(slot, dict):
            continue
        idx = _safe_i64(slot.get('index', 0), 0)
        prev_slot = previous_slots.get(<int>idx) if previous_slots is not None else None
        slot['ready_edge'] = bool(prev_slot and slot_ready(slot) and not slot_ready(prev_slot))
        if slot['ready_edge']:
            edges += 1
    return edges


cpdef int resolve_skill_level_id_from_cd(object base_skill_id_obj,
                                         object cd_map,
                                         object last_use_map=None,
                                         object seen_ids=None):
    """Resolve a base skill id to a skill_level_id from cooldown/history maps."""
    cdef long long base_skill_id = _safe_i64(base_skill_id_obj, 0)
    cdef object slid
    cdef long long skill_level_id
    if base_skill_id <= 0:
        return 0
    for slid in (cd_map or {}):
        skill_level_id = _safe_i64(slid, 0)
        if skill_level_id > 0 and skill_level_id // 100 == base_skill_id:
            return <int>skill_level_id
    for slid in (last_use_map or {}):
        skill_level_id = _safe_i64(slid, 0)
        if skill_level_id > 0 and skill_level_id // 100 == base_skill_id:
            return <int>skill_level_id
    for slid in (seen_ids or []):
        skill_level_id = _safe_i64(slid, 0)
        if skill_level_id > 0 and skill_level_id // 100 == base_skill_id:
            return <int>skill_level_id
    return 0


cpdef tuple stabilize_packet_stamina(object current_obj,
                                     object stamina_max_obj,
                                     object energy_priority_obj,
                                     object stable_sta_current_obj,
                                     object stable_sta_max_obj,
                                     object pending_sta_current_obj,
                                     object pending_sta_hits_obj):
    """Advance the packet STA spike filter state machine."""
    cdef int current = <int>_safe_i64(current_obj, 0)
    cdef int stamina_max = <int>_safe_i64(stamina_max_obj, 0)
    cdef int energy_priority = <int>_safe_i64(energy_priority_obj, 0)
    cdef int stable_sta_current = <int>_safe_i64(stable_sta_current_obj, 0)
    cdef int stable_sta_max = <int>_safe_i64(stable_sta_max_obj, 0)
    cdef int pending_sta_current = <int>_safe_i64(pending_sta_current_obj, 0)
    cdef int pending_sta_hits = <int>_safe_i64(pending_sta_hits_obj, 0)
    cdef int previous
    cdef int threshold
    cdef bint has_pending = pending_sta_current_obj is not None
    cdef bint accepted_repeated = False
    cdef bint held_spike = False
    if stamina_max <= 0:
        if current < 0:
            current = 0
        return (current, 0, 0, None, 0, accepted_repeated, held_spike)

    if current < 0:
        current = 0
    elif current > stamina_max:
        current = stamina_max

    if stable_sta_max != stamina_max:
        if stable_sta_current < 0:
            stable_sta_current = 0
        elif stable_sta_current > stamina_max:
            stable_sta_current = stamina_max
        stable_sta_max = stamina_max
        has_pending = False
        pending_sta_current = 0
        pending_sta_hits = 0

    if energy_priority != 2:
        return (current, current, stable_sta_max, None, 0, accepted_repeated, held_spike)

    previous = stable_sta_current
    if previous < 0:
        previous = 0
    elif previous > stamina_max:
        previous = stamina_max
    if previous <= 0:
        return (current, current, stable_sta_max, pending_sta_current_obj if has_pending else None,
                pending_sta_hits, accepted_repeated, held_spike)

    threshold = <int>(stamina_max * 0.08)
    if threshold < 36:
        threshold = 36
    if abs(current - previous) <= threshold:
        return (current, current, stable_sta_max, None, 0, accepted_repeated, held_spike)

    if has_pending and pending_sta_current == current:
        pending_sta_hits += 1
    else:
        pending_sta_current = current
        pending_sta_hits = 1
        has_pending = True

    if pending_sta_hits >= 2:
        accepted_repeated = True
        return (current, current, stable_sta_max, None, 0, accepted_repeated, held_spike)

    held_spike = True
    return (previous, stable_sta_current, stable_sta_max,
            pending_sta_current if has_pending else None,
            pending_sta_hits, accepted_repeated, held_spike)


cpdef tuple compute_skill_cd_ui_state(object total_ms_obj,
                                      object elapsed_ms_obj,
                                      object charge_count_obj,
                                      object pkt_sub_ratio_obj,
                                      object pkt_sub_fixed_obj,
                                      object pkt_accel_obj,
                                      object ent_cd_flat_obj,
                                      object ent_cd_pct_obj,
                                      object ent_accel_obj,
                                      object tmp_cd_pct_obj,
                                      object tmp_cd_fixed_obj,
                                      object tmp_accel_obj,
                                      object now_server_ms_obj,
                                      object now_local_ms_obj,
                                      object begin_ms_obj,
                                      object last_update_ms_obj,
                                      object parser_speed_obj,
                                      object last_use_time_obj,
                                      object now_t_obj):
    """Compute packet skill cooldown UI values from raw numeric fields."""
    cdef int total_ms = <int>_safe_i64(total_ms_obj, 0)
    cdef int elapsed_ms = <int>_safe_i64(elapsed_ms_obj, 0)
    cdef int charge_count = <int>_safe_i64(charge_count_obj, 0)
    cdef int pkt_sub_ratio = <int>_safe_i64(pkt_sub_ratio_obj, 0)
    cdef int pkt_sub_fixed = <int>_safe_i64(pkt_sub_fixed_obj, 0)
    cdef int pkt_accel = <int>_safe_i64(pkt_accel_obj, 0)
    cdef int ent_cd_flat = <int>_safe_i64(ent_cd_flat_obj, 0)
    cdef int ent_cd_pct = <int>_safe_i64(ent_cd_pct_obj, 0)
    cdef int ent_accel = <int>_safe_i64(ent_accel_obj, 0)
    cdef int tmp_cd_pct = <int>_safe_i64(tmp_cd_pct_obj, 0)
    cdef int tmp_cd_fixed = <int>_safe_i64(tmp_cd_fixed_obj, 0)
    cdef int tmp_accel = <int>_safe_i64(tmp_accel_obj, 0)
    cdef int now_server_ms = <int>_safe_i64(now_server_ms_obj, 0)
    cdef int now_local_ms = <int>_safe_i64(now_local_ms_obj, 0)
    cdef int begin_ms = <int>_safe_i64(begin_ms_obj, 0)
    cdef int last_update_ms = <int>_safe_i64(last_update_ms_obj, 0)
    cdef double parser_speed = _safe_f64(parser_speed_obj, 0.0)
    cdef double last_use_time = _safe_f64(last_use_time_obj, 0.0)
    cdef double now_t = _safe_f64(now_t_obj, 0.0)
    cdef int effective_ms = total_ms
    cdef double accel_rate = 0.0
    cdef double total_pct
    cdef int total_flat
    cdef bint has_entity_mods
    cdef bint has_buff_mods
    cdef bint has_pkt_mods
    cdef double accel_speed_mult
    cdef int server_elapsed_ms = 0
    cdef bint has_server_clock
    cdef double vcd_speed
    cdef double measured_vcd_speed = 0.0
    cdef double current_vcd
    cdef double remaining_vcd
    cdef int remaining_ms = 0
    cdef double cooldown_pct = 0.0
    cdef int display_total_ms = 0
    cdef int real_total_cd = 0
    cdef int local_since_ms = 0
    cdef int local_elapsed = 0
    cdef double source_confidence = 0.0
    cdef bint active = False
    cdef str state
    cdef long long _ts_2020 = 1577836800000
    cdef long long _ts_2030 = 1893456000000

    if charge_count < 0:
        charge_count = 0
    if total_ms > 600000 or total_ms < 0:
        total_ms = 0
        effective_ms = 0
    if elapsed_ms > 600000 or elapsed_ms < 0:
        elapsed_ms = 0

    has_entity_mods = ent_cd_flat > 0 or ent_cd_pct > 0 or ent_accel > 0
    has_buff_mods = tmp_cd_pct > 0 or tmp_cd_fixed > 0 or tmp_accel > 0
    has_pkt_mods = pkt_sub_ratio > 0 or pkt_sub_fixed > 0 or pkt_accel > 0

    if has_entity_mods or has_buff_mods:
        total_pct = (ent_cd_pct + tmp_cd_pct) / 10000.0
        total_flat = ent_cd_flat + tmp_cd_fixed
        effective_ms = <int>max(0.0, (1.0 - total_pct) * (total_ms - total_flat))
        accel_rate = (ent_accel + tmp_accel) / 10000.0
        if pkt_sub_fixed > 0 or pkt_sub_ratio > 0:
            effective_ms = max(0, effective_ms - pkt_sub_fixed)
            if pkt_sub_ratio > 0:
                effective_ms = <int>(effective_ms * max(0, 10000 - pkt_sub_ratio) / 10000)
        if pkt_accel > 0:
            accel_rate += pkt_accel / 10000.0
    elif has_pkt_mods:
        if pkt_sub_fixed > 0 or pkt_sub_ratio > 0:
            effective_ms = max(0, total_ms - pkt_sub_fixed)
            if pkt_sub_ratio > 0:
                effective_ms = <int>(effective_ms * max(0, 10000 - pkt_sub_ratio) / 10000)
        if pkt_accel > 0:
            accel_rate = pkt_accel / 10000.0

    accel_speed_mult = 1.0 + accel_rate
    vcd_speed = accel_speed_mult
    has_server_clock = (now_server_ms > 0 and _ts_2020 < begin_ms < _ts_2030)
    if has_server_clock:
        server_elapsed_ms = max(0, now_server_ms - begin_ms)

    if elapsed_ms > 0 and server_elapsed_ms > 500:
        measured_vcd_speed = elapsed_ms / <double>server_elapsed_ms
        if 0.5 <= measured_vcd_speed <= 25.0:
            vcd_speed = measured_vcd_speed
    if measured_vcd_speed <= 0.0 and 0.8 <= parser_speed <= 25.0:
        vcd_speed = parser_speed

    if total_ms > 0:
        if last_update_ms <= 0:
            last_update_ms = now_local_ms
        local_since_ms = max(0, now_local_ms - last_update_ms)
        if elapsed_ms > 0 and vcd_speed > 0.01:
            current_vcd = elapsed_ms + local_since_ms * vcd_speed
            if current_vcd >= total_ms:
                remaining_ms = 0
                cooldown_pct = 0.0
            else:
                remaining_vcd = total_ms - current_vcd
                remaining_ms = max(0, <int>(remaining_vcd / vcd_speed))
                display_total_ms = max(1, <int>(total_ms / vcd_speed))
                cooldown_pct = (<double>remaining_ms / <double>display_total_ms) if display_total_ms > 0 else 0.0
                if cooldown_pct < 0.0:
                    cooldown_pct = 0.0
                elif cooldown_pct > 1.0:
                    cooldown_pct = 1.0
            source_confidence = 0.97
            display_total_ms = max(1, <int>(total_ms / vcd_speed))
        elif has_server_clock:
            real_total_cd = <int>(effective_ms / accel_speed_mult) if accel_speed_mult > 0.01 else effective_ms
            if server_elapsed_ms >= real_total_cd:
                remaining_ms = 0
                cooldown_pct = 0.0
            elif server_elapsed_ms >= 0:
                remaining_ms = max(0, real_total_cd - server_elapsed_ms)
                cooldown_pct = (<double>remaining_ms / <double>real_total_cd) if real_total_cd > 0 else 0.0
                if cooldown_pct < 0.0:
                    cooldown_pct = 0.0
                elif cooldown_pct > 1.0:
                    cooldown_pct = 1.0
            else:
                remaining_ms = real_total_cd
                cooldown_pct = 1.0
            display_total_ms = max(1, real_total_cd)
            source_confidence = 0.80
        else:
            local_elapsed = max(0, now_local_ms - last_update_ms)
            real_total_cd = <int>(effective_ms / accel_speed_mult) if accel_speed_mult > 0.01 else effective_ms
            remaining_ms = max(0, real_total_cd - local_elapsed)
            display_total_ms = max(1, real_total_cd)
            cooldown_pct = (<double>remaining_ms / <double>display_total_ms) if display_total_ms > 0 else 0.0
            if cooldown_pct < 0.0:
                cooldown_pct = 0.0
            elif cooldown_pct > 1.0:
                cooldown_pct = 1.0
            source_confidence = 0.55
        effective_ms = display_total_ms
        if charge_count > 0:
            cooldown_pct = 0.0
        active = remaining_ms > 0 and (now_t - last_use_time) <= 0.45

    if charge_count > 0 or (remaining_ms <= 120 and cooldown_pct <= 0.02):
        state = 'ready'
    elif active:
        state = 'active'
    elif remaining_ms > 0 and cooldown_pct > 0.02:
        state = 'cooldown'
    else:
        state = 'ready'

    return (
        state,
        _round_pos_i32(cooldown_pct * 1000.0) / 1000.0,
        max(0, remaining_ms),
        max(0, effective_ms or total_ms),
        charge_count,
        bool(active),
        _round_pos_i32(source_confidence * 100.0) / 100.0,
    )


cpdef tuple collect_meaningful_skill_ids(object cd_map,
                                         object seen_ids=None,
                                         object last_use_map=None):
    """Return ``(all_ids_tuple, deduped_ids_tuple)`` for packet skill inference."""
    cdef set all_ids = set()
    cdef list meaningful = []
    cdef dict seen_base = {}
    cdef object slid_obj
    cdef object cd_info
    cdef object get_fn
    cdef long long slid
    cdef long long base
    cdef long long prev
    cdef bint prev_in_cd
    cdef bint slid_in_cd

    for slid_obj in (cd_map or {}):
        slid = _safe_i64(slid_obj, 0)
        if slid > 0:
            all_ids.add(<int>slid)
    for slid_obj in (seen_ids or []):
        slid = _safe_i64(slid_obj, 0)
        if slid > 0:
            all_ids.add(<int>slid)
    for slid_obj in (last_use_map or {}):
        slid = _safe_i64(slid_obj, 0)
        if slid > 0:
            all_ids.add(<int>slid)

    if not all_ids:
        return ((), ())

    for slid in sorted(all_ids):
        cd_info = (cd_map or {}).get(slid) or (cd_map or {}).get(<int>slid)
        get_fn = cd_info.get if cd_info is not None and hasattr(cd_info, 'get') else None
        if get_fn is not None and _safe_i64(get_fn('duration', 0), 0) > 0:
            meaningful.append(<int>slid)
        elif slid in (last_use_map or {}):
            meaningful.append(<int>slid)

    if not meaningful:
        return (tuple(sorted(all_ids)), ())

    for slid_obj in meaningful:
        slid = _safe_i64(slid_obj, 0)
        if slid <= 0:
            continue
        base = slid // 100 if slid >= 100 else slid
        prev = _safe_i64(seen_base.get(<int>base), 0)
        if prev <= 0:
            seen_base[<int>base] = <int>slid
            continue
        prev_in_cd = prev in (cd_map or {})
        slid_in_cd = slid in (cd_map or {})
        if ((slid_in_cd and not prev_in_cd)
                or (slid_in_cd == prev_in_cd and slid > prev)):
            seen_base[<int>base] = <int>slid

    return (
        tuple(sorted(all_ids)),
        tuple(sorted(seen_base.values())),
    )


cpdef tuple infer_skill_slot_map(object deduped_ids,
                                 object profession_id_obj,
                                 object normal_attack_base_obj,
                                 object prof_skill_bases,
                                 object ultimate_base_obj,
                                 object skill_to_profession):
    """Return ``(pinned_skill_base, slot_map_dict)`` for inferred packet skills."""
    cdef int profession_id = <int>_safe_i64(profession_id_obj, 0)
    cdef int normal_attack_base = <int>_safe_i64(normal_attack_base_obj, 0)
    cdef int ultimate_base = <int>_safe_i64(ultimate_base_obj, 0)
    cdef int pinned_normal = 0
    cdef int pinned_skill = 0
    cdef int pinned_skill_base = 0
    cdef int pinned_ultimate = 0
    cdef list rest = []
    cdef object slid_obj
    cdef int slid
    cdef int base
    cdef int assigned_profession
    cdef dict slot_map = {}
    cdef list fill_positions
    cdef list filtered_rest = []
    cdef int prof_lookup
    if not deduped_ids:
        return (0, {})

    for slid_obj in deduped_ids:
        slid = <int>_safe_i64(slid_obj, 0)
        if slid <= 0:
            continue
        base = slid // 100 if slid >= 100 else slid
        if normal_attack_base > 0 and base == normal_attack_base and pinned_normal <= 0:
            pinned_normal = slid
        elif prof_skill_bases and base in prof_skill_bases and pinned_skill <= 0:
            pinned_skill = slid
            pinned_skill_base = base
        elif ultimate_base > 0 and base == ultimate_base and pinned_ultimate <= 0:
            pinned_ultimate = slid
        else:
            rest.append(slid)

    if profession_id > 0 and rest:
        for slid in rest:
            base = slid // 100 if slid >= 100 else slid
            prof_lookup = <int>_safe_i64(skill_to_profession.get(base, 0), 0)
            if prof_lookup > 0 and prof_lookup != profession_id:
                continue
            filtered_rest.append(slid)
        rest = filtered_rest

    rest.sort()
    if pinned_normal > 0:
        slot_map[1] = pinned_normal
    if pinned_skill > 0:
        slot_map[2] = pinned_skill
    if pinned_ultimate > 0:
        slot_map[7] = pinned_ultimate

    fill_positions = [i for i in [3, 4, 5, 6, 8, 9] if i not in slot_map]
    for slid in rest:
        if not fill_positions:
            break
        slot_map[fill_positions.pop(0)] = slid

    if pinned_normal <= 0 and pinned_skill <= 0 and pinned_ultimate <= 0:
        slot_map = {}
        for base, slid in enumerate(sorted(deduped_ids)[:9], start=1):
            slot_map[base] = <int>_safe_i64(slid, 0)
    return (pinned_skill_base, slot_map)


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
