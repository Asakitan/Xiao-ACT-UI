# -*- coding: utf-8 -*-
"""
SAO Auto — Npcap 网络抓包 + TCP 流重组

通过 ctypes 调用 wpcap.dll (Npcap/WinPcap)，抓取以太网帧，
解析 IP/TCP 层，按序列号重组 TCP 流，输出完整的游戏协议帧。
"""

import struct
import threading
import time
import ctypes
import ctypes.wintypes
import logging
import os
from typing import Optional, Callable, List, Dict, Tuple

logger = logging.getLogger('sao_auto.capture')

try:
    import _sao_cy_packet as _CY_PACKET
except Exception:  # noqa: BLE001
    _CY_PACKET = None

# ═══════════════════════════════════════════════
#  Npcap / WinPcap ctypes 绑定
# ═══════════════════════════════════════════════

_wpcap_dll = None


def _load_wpcap():
    """懒加载 wpcap.dll"""
    global _wpcap_dll
    if _wpcap_dll is not None:
        return _wpcap_dll
    # Npcap 优先 (安装在 System32\Npcap\)
    npcap_dir = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'),
                             'System32', 'Npcap')
    for path in [os.path.join(npcap_dir, 'wpcap.dll'), 'wpcap.dll']:
        try:
            _wpcap_dll = ctypes.cdll.LoadLibrary(path)
            logger.info(f'[Capture] Loaded: {path}')
            return _wpcap_dll
        except OSError:
            continue
    raise RuntimeError('无法加载 wpcap.dll — 请安装 Npcap (https://npcap.com)')


class _PcapIf(ctypes.Structure):
    """pcap_if_t 前四个字段"""
    pass


_PcapIf._fields_ = [
    ('next', ctypes.POINTER(_PcapIf)),
    ('name', ctypes.c_char_p),
    ('description', ctypes.c_char_p),
    ('addresses', ctypes.c_void_p),
    ('flags', ctypes.c_uint32),
]


class _PcapPkthdr(ctypes.Structure):
    _fields_ = [
        ('tv_sec', ctypes.c_long),
        ('tv_usec', ctypes.c_long),
        ('caplen', ctypes.c_uint32),
        ('len', ctypes.c_uint32),
    ]


# ═══════════════════════════════════════════════
#  设备发现
# ═══════════════════════════════════════════════

def list_devices() -> List[Dict[str, str]]:
    """列出所有 Npcap 网络设备，返回 [{name, description}]"""
    dll = _load_wpcap()
    alldevs = ctypes.POINTER(_PcapIf)()
    errbuf = ctypes.create_string_buffer(256)
    ret = dll.pcap_findalldevs(ctypes.byref(alldevs), errbuf)
    if ret != 0:
        raise RuntimeError(f'pcap_findalldevs failed: {errbuf.value.decode("utf-8", "ignore")}')
    devices = []
    cur = alldevs
    while cur:
        name = cur.contents.name
        desc = cur.contents.description
        if name:
            n = name.decode('utf-8', 'ignore')
            d = desc.decode('utf-8', 'ignore') if desc else n
            # 只保留 NPF 设备
            if '\\Device\\NPF_' in n or 'NPF_' in n:
                devices.append({'name': n, 'description': d})
        nxt = cur.contents.next
        cur = nxt if nxt else None
    dll.pcap_freealldevs(alldevs)
    return devices


def auto_select_device() -> Optional[Dict[str, str]]:
    """自动选择默认网络设备（排除虚拟适配器，优选真实网卡）"""
    devs = list_devices()
    if not devs:
        return None

    # 打印所有设备供诊断
    print(f'[Capture] 发现 {len(devs)} 个 NPF 设备:', flush=True)
    for i, d in enumerate(devs):
        print(f'  [{i}] {d["description"]}  ({d["name"]})', flush=True)

    # 排除虚拟 / 不会有实际流量的适配器
    virtual_keywords = [
        'vmware', 'virtualbox', 'hyper-v', 'zerotier',
        'docker', 'wsl', 'vethernet', 'loopback',
        'npcap loopback', 'bluetooth',
        'wan miniport', 'network monitor', 'miniport',
        'microsoft kernel debug', 'teredo', 'isatap', '6to4',
        'pptp', 'l2tp', 'sstp', 'pppoe', 'ikev2',
        'tunnel', 'tap-windows', 'wireguard', 'vpn',
        'pseudo', 'microsoft wi-fi direct',
    ]
    real_devs = []
    for d in devs:
        desc_low = d['description'].lower()
        if not any(kw in desc_low for kw in virtual_keywords):
            real_devs.append(d)

    candidates = real_devs if real_devs else devs

    # 优选真实物理网卡（关键词打分）
    nic_keywords = [
        'ethernet', 'wi-fi', 'wifi', 'wireless', '802.11',
        'realtek', 'intel', 'broadcom', 'qualcomm', 'killer',
        'mediatek', 'marvell', 'aquantia', 'nvidia',
        'gigabit', 'gaming',
    ]

    def _score(d: Dict[str, str]) -> int:
        desc_low = d['description'].lower()
        return sum(1 for kw in nic_keywords if kw in desc_low)

    candidates.sort(key=_score, reverse=True)

    chosen = candidates[0]
    print(f'[Capture] 自动选择: {chosen["description"]}', flush=True)
    return chosen


# ═══════════════════════════════════════════════
#  IP 分片重组
# ═══════════════════════════════════════════════

class _IpFragmentCache:
    """IPv4 分片缓存，30 秒超时自动清理"""
    TIMEOUT = 30.0

    def __init__(self):
        self._cache: Dict[str, Dict] = {}

    def feed(self, ip_id: int, src: bytes, dst: bytes, proto: int,
             frag_offset: int, more_frag: bool, payload: bytes, total_len: int
             ) -> Optional[bytes]:
        key = f'{ip_id}-{src.hex()}-{dst.hex()}-{proto}'
        now = time.time()
        # 清理过期
        expired = [k for k, v in self._cache.items() if now - v['ts'] > self.TIMEOUT]
        for k in expired:
            del self._cache[k]

        if key not in self._cache:
            self._cache[key] = {'fragments': {}, 'ts': now, 'done': False}
        entry = self._cache[key]
        entry['fragments'][frag_offset] = payload
        entry['ts'] = now
        if not more_frag:
            entry['last_offset'] = frag_offset
            entry['total'] = frag_offset * 8 + len(payload)

        # 检查是否所有分片都已到达
        if 'total' in entry:
            buf = bytearray(entry['total'])
            covered = 0
            for off, data in sorted(entry['fragments'].items()):
                start = off * 8
                buf[start:start + len(data)] = data
                covered += len(data)
            if covered >= entry['total']:
                del self._cache[key]
                return bytes(buf)
        return None


# ═══════════════════════════════════════════════
#  以太网 / IP / TCP 手动解析 (无 dpkt 依赖)
# ═══════════════════════════════════════════════

def _parse_eth_ip_tcp(raw: bytes) -> Optional[Tuple[bytes, bytes, int, int, int, bytes, int, int, bool]]:
    """
    解析以太网帧 → IPv4 → TCP。
    返回 (src_ip, dst_ip, sport, dport, seq, payload, ip_id, frag_offset, more_frag)
    """
    if len(raw) < 54:
        return None
    # Ethernet
    eth_type = struct.unpack_from('!H', raw, 12)[0]
    if eth_type != 0x0800:  # 非 IPv4
        return None
    ip_off = 14
    # IPv4
    ver_ihl = raw[ip_off]
    ihl = (ver_ihl & 0xF) * 4
    if ihl < 20 or ip_off + ihl > len(raw):
        return None
    total_len = struct.unpack_from('!H', raw, ip_off + 2)[0]
    ip_id = struct.unpack_from('!H', raw, ip_off + 4)[0]
    flags_frag = struct.unpack_from('!H', raw, ip_off + 6)[0]
    more_frag = bool(flags_frag & 0x2000)
    frag_offset = flags_frag & 0x1FFF
    proto = raw[ip_off + 9]
    src_ip = raw[ip_off + 12:ip_off + 16]
    dst_ip = raw[ip_off + 16:ip_off + 20]

    if proto != 6:  # 非 TCP
        # 但如果是分片，仍需缓存
        return (src_ip, dst_ip, 0, 0, 0, raw[ip_off + ihl:ip_off + total_len],
                ip_id, frag_offset, more_frag)

    tcp_off = ip_off + ihl
    if tcp_off + 20 > len(raw):
        return None
    sport, dport, seq = struct.unpack_from('!HHI', raw, tcp_off)
    data_offset = ((raw[tcp_off + 12] >> 4) & 0xF) * 4
    payload_off = tcp_off + data_offset
    payload = raw[payload_off:ip_off + total_len] if payload_off < len(raw) else b''

    return (src_ip, dst_ip, sport, dport, seq, payload,
            ip_id, frag_offset, more_frag)


# ═══════════════════════════════════════════════
#  TCP 流重组器
# ═══════════════════════════════════════════════

C3SB_SIGNATURE = b'\x00\x63\x33\x53\x42\x00'
C3SB_SHORT = b'\x63\x33\x53\x42'


class TcpReassembler:
    """
    单向 TCP 流重组 + 游戏帧提取。
    识别游戏服务器后，对下行流重组并按 [4B-size][payload] 切割游戏帧。
    支持场景服务器切换检测 (切换地图/副本时游戏连接新的场景服务器)。
    """

    def __init__(self, on_game_packet: Callable[[bytes], None],
                 on_server_change: Optional[Callable[[], None]] = None):
        self._on_pkt = on_game_packet  # 回调: 一个完整游戏帧
        self._on_server_change = on_server_change  # 回调: 场景服务器切换
        self._server_addr: Optional[str] = None
        self._lock = threading.Lock()

        # TCP seq 重组
        self._next_seq: int = -1
        self._cache: Dict[int, bytes] = {}
        self._buf = b''
        self._last_t: float = 0
        self._gap_since: float = 0.0  # 首次检测到缺段间隙的时间戳

        # IP 分片
        self.stats = {
            'raw_frames': 0,
            'tcp_segments': 0,
            'complete_game_frames': 0,
            'seq_resets': 0,
            'cache_overflows': 0,
            'gap_skips': 0,
            'server_changes': 0,
            'replayed_after_change': 0,
        }
        self._frag = _IpFragmentCache()
        # v2.1.18: 同 (addr) 入站 payload 的近端环形缓冲, 用于在
        # 检测到 server change / 同服重连时回放最近几个未被消费的包.
        # 切场景时游戏会先发若干 TCP 段, 直到出现含 c3SB 的关键帧后我们才识别出新
        # server, 之前的段如果 (1) 来自旧 server addr → 直接被 `addr != server_addr`
        # 短路丢弃, (2) 来自新 server addr 但还没识别 → 被 `_try_identify` False 丢弃.
        # 缓存最近的 (addr,seq,payload) 让识别成功后能补喂.
        self._recent_pkts: list = []  # list[(addr, seq, payload)]
        self._RECENT_PKT_LIMIT = 24
        # v2.3.7: 同服重连冷却 — 真实重连是单次事件, 不可能几秒内连发.
        # 旧版本只要 _seq_anomalous + (loose c3SB 或 _looks_like_frame_start)
        # 就触发, 误识率不为零, 一旦在繁忙流上误中, _next_seq 被重置 -1, 下一
        # 个被缓存的乱序包又落入异常区间, 又触发, 形成"⚡ 检测到同服重连"刷屏
        # → BossHP 目标被反复清掉. 这里加最小间隔, 真重连漏不了, 误触发被吃掉.
        self._last_reconnect_ts: float = 0.0
        self._RECONNECT_COOLDOWN = 3.0

    @property
    def server_identified(self) -> bool:
        return self._server_addr is not None

    def reset(self):
        with self._lock:
            self._server_addr = None
            self._next_seq = -1
            self._cache.clear()
            self._buf = b''
            self._last_t = 0
            self._gap_since = 0.0

    # ─── 主入口 ───
    def feed_raw_frame(self, raw: bytes):
        """接收一个原始以太网帧"""
        self.stats['raw_frames'] += 1
        parsed = _parse_eth_ip_tcp(raw)
        if parsed is None:
            return
        src_ip, dst_ip, sport, dport, seq, payload, ip_id, frag_off, mf = parsed

        # IP 分片重组
        if mf or frag_off > 0:
            total_len = len(payload) + frag_off * 8 if not mf else 0
            reassembled = self._frag.feed(
                ip_id, src_ip, dst_ip, 6, frag_off, mf, payload, total_len)
            if reassembled is None:
                return
            # 重新解析 TCP 头 from 完整 IP payload
            if len(reassembled) < 20:
                return
            sport, dport, seq = struct.unpack_from('!HHI', reassembled, 0)
            data_offset = ((reassembled[12] >> 4) & 0xF) * 4
            payload = reassembled[data_offset:]

        if not payload:
            return

        addr = f'{src_ip.hex()}:{sport}'

        # v2.1.18: 缓存最近的入站包, 供 server-change / 同服重连后回放.
        # 必须在所有早返回路径之前记录, 这样未被消费的包才能在重连后被找回.
        self._recent_pkts.append((addr, seq, payload))
        if len(self._recent_pkts) > self._RECENT_PKT_LIMIT:
            self._recent_pkts.pop(0)

        # ─── 服务器识别 ───
        if self._server_addr is None:
            # 初次识别允许松散 c3SB (无锚点, 必须接受首个候选).
            if self._try_identify(payload, addr):
                self._server_addr = addr
                logger.info(f'[Capture] 识别到游戏服务器: {_fmt_ip(src_ip)}:{sport}')
                # v2.1.18: 回放在识别成功之前缓存的同 addr 包,
                # 拿回首个 SyncContainerData / EnterGame 等关键登录帧.
                replayed = self._replay_recent_for_addr(addr, exclude_seq=seq)
                if replayed:
                    print(
                        f'[Capture] 回放 {replayed} 个识别前缓存包 (initial)',
                        flush=True,
                    )
                # 首包也要喂入 TCP 重组, 可能含 SyncContainerData 等关键数据
                self._feed_tcp(seq, payload)
            return

        if addr != self._server_addr:
            # ─── 场景服务器切换检测 ───
            # 切换地图/副本时，游戏会连接新的场景服务器。
            # v2.3.6: 必须用 *严格* 识别 (FrameDown 嵌套 c3SB 或 Login Return).
            #   松散 c3SB 字面量在非游戏 TCP 流 (聊天/社交/CDN/语音) 中随机
            #   命中概率不低, 旧逻辑会把 _server_addr 偷换到误识别地址 →
            #   _on_scene_change → 清掉 BossHP 目标 → 下一个真游戏包又被
            #   判为 "跨 addr c3SB" 切回去, 形成 ping-pong, 用户看到 BossHP
            #   反复刷新最终失踪.
            if self._identify_strict(payload):
                # v2.3.15: 数据流没有断的时候，不触发场景切换。
                # 如果旧服务器在最近 3 秒内还在持续提供有效游戏帧，
                # 那说明旧连接仍然活跃，这个新 addr 的包很可能是干扰
                # 流量 (聊天/社交/语音等其他 TCP 连接碰巧带了 c3SB 特征)。
                # 只有旧服务器真的停了 (超时无数据) 才切换。
                _now = time.time()
                _old_still_alive = (_now - self._last_t) < 3.0
                if _old_still_alive:
                    # 旧流仍在活跃, 不切换 — 直接丢弃新 addr 的包
                    return
                old_addr = self._server_addr
                self._server_addr = addr
                # 重置 TCP 重组状态
                with self._lock:
                    self._next_seq = -1
                    self._cache.clear()
                    self._buf = b''
                    self._gap_since = 0.0
                self.stats['server_changes'] += 1
                logger.info(
                    f'[Capture] 场景服务器切换: {old_addr} → {addr} '
                    f'({_fmt_ip(src_ip)}:{sport})'
                )
                print(
                    f'[Capture] ⚡ 场景服务器切换 → {_fmt_ip(src_ip)}:{sport} '
                    f'(第 {self.stats["server_changes"]} 次)',
                    flush=True,
                )
                # 通知上层: 场景已切换，需要清理旧数据
                if self._on_server_change:
                    try:
                        self._on_server_change()
                    except Exception as e:
                        logger.error(f'[Capture] on_server_change callback error: {e}')
                # v2.1.18: 回放最近缓存的来自新 server 的包 (按 seq 升序),
                # 找回切换瞬间被丢掉的 SyncContainerData / SyncToMeDelta.
                replayed = self._replay_recent_for_addr(addr, exclude_seq=seq)
                if replayed:
                    print(
                        f'[Capture] 回放 {replayed} 个新场景缓存包 (server change)',
                        flush=True,
                    )
                # 继续处理新服务器的首个包
                self._feed_tcp(seq, payload)
            return

        # ─── 同服重连检测 ───
        # 游戏重新登录到同一服务器时, TCP 连接重建, seq 号完全不同。
        # 之前: 必须含 c3SB 签名且 seq 不匹配才触发. 但快速反复进入同一
        # 地图 (例如爬塔) 时, 重连首包不一定带 c3SB —— 我们就丢了重连
        # 检测, 后续 SyncContainerData 被旧 _next_seq 顶到 cache 等 2 秒
        # gap-skip 才恢复, 期间解析器 _current_uuid stale → DPS=0 / 伤害
        # 不计算. 修复: 放宽到 "seq 偏差异常大 + 数据看起来像新游戏帧
        # 起点" 也算重连, 不再强求 c3SB.
        #
        # TCP 接收窗口典型 64KB-1MB, 任何 seq 偏差远大于该量级
        # (双向都 > 1MB) 必为新 ISN, 不可能是合法的乱序/重传/窗口移动.
        #
        # v2.3.4: 关键修复 — 之前只要 c3SB 签名出现在乱序 TCP 包里
        # 就触发重连, 误杀严重. mid-fight 任何包含 4 字节 'c3SB' 的
        # ZSTD 数据 / 玩家名 / buff icon ID 都会让 BossHP 浮窗瞬间消失
        # (_on_scene_change → _bb_last_target_uuid=0 → BossHP hide).
        # 现在所有触发路径都强制要求 ``_seq_anomalous`` (双向 > 1MB).
        # 这样 mid-stream 乱序段 (TCP 窗口内 ≤ 1MB) 不会再触发误重连.
        with self._lock:
            _seq_match = (self._next_seq == -1 or
                          self._next_seq == seq or
                          seq in self._cache)
            _seq_anomalous = False
            if not _seq_match and self._next_seq != -1:
                _diff_fwd = (seq - self._next_seq) & 0xFFFFFFFF
                _diff_bwd = (self._next_seq - seq) & 0xFFFFFFFF
                # 双向都远超 TCP 窗口 → 必为新连接的 ISN.
                _seq_anomalous = (_diff_fwd > 1_000_000
                                  and _diff_bwd > 1_000_000)
        # v2.3.7: 收紧重连判定 — 必须 *严格* 识别 (FrameDown 嵌套 c3SB 或
        #   Login Return), 不再接受松散 c3SB 字面量, 也不再单凭 4B BE 帧头
        #   ([6,999999], 误中率 ≈0.023%/包, busy stream 上每秒就能误触发,
        #   把 BossHP 反复清掉).
        # v2.3.4: 必须 seq 异常 + 严格游戏签名.
        _is_reconnect = (
            (not _seq_match)
            and _seq_anomalous
            and self._identify_strict(payload)
        )
        # v2.3.7: 冷却窗口 — 真实重连是单次事件, N 秒内重复触发一律视为误识.
        if _is_reconnect:
            _now = time.time()
            if (_now - self._last_reconnect_ts) < self._RECONNECT_COOLDOWN:
                _is_reconnect = False
            else:
                self._last_reconnect_ts = _now
        if _is_reconnect:
            with self._lock:
                logger.info(
                    f'[Capture] 同服重连检测: seq 不匹配 '
                    f'(期望 {self._next_seq}, 收到 {seq}, '
                    f'anomalous={_seq_anomalous}), 重置 TCP 流'
                )
                print(
                    f'[Capture] ⚡ 检测到同服重连 — 重置 TCP 流',
                    flush=True,
                )
                self._next_seq = -1
                self._cache.clear()
                self._buf = b''
                self._gap_since = 0.0
            # 通知上层: 等同于场景服务器切换
            if self._on_server_change:
                try:
                    self._on_server_change()
                except Exception as e:
                    logger.error(f'[Capture] on_server_change callback error: {e}')
            # v2.1.18: 同服重连后也要回放, 重连瞬间被 _seq_match 短路丢掉的
            # SyncContainerData (relogin full sync) 必须在这里追回, 否则
            # _current_uid / 角色装备 / dungeon 等依赖 full sync 的状态
            # 永远停留在 stale 缓存上.
            # v2.3.3: 限定回放窗口为新 ISN 附近 1MB, 排除重连前的旧 ISN 包.
            # 否则旧 seq 会污染 _next_seq, 导致后续真新包再次触发"重连",
            # 形成无限循环.
            replayed = self._replay_recent_for_addr(
                addr, exclude_seq=seq, seq_window=1_000_000)
            if replayed:
                print(
                    f'[Capture] 回放 {replayed} 个缓存包 (same-server reconnect)',
                    flush=True,
                )

        # ─── TCP 重组 ───
        self._feed_tcp(seq, payload)

    def _replay_recent_for_addr(self, addr: str, exclude_seq: int,
                                 seq_window: int = 0) -> int:
        """v2.1.18: 把 _recent_pkts 里属于该 addr、seq 不等于 exclude_seq 的包按 seq
        升序回放给 _feed_tcp, 用于 server-change / 同服重连后追回切换瞬间被丢的包.

        v2.3.3: 新增 ``seq_window`` 参数. 同服重连场景中, ``_recent_pkts``
        会同时包含**重连前**(旧 ISN seq) 和**重连后**(新 ISN seq) 的包,
        addr 完全相同无法区分. 若不过滤, replay 会把旧 ISN seq 喂入
        ``_feed_tcp``, ``_next_seq`` 落到旧 ISN 区域, 后续真正的新 ISN 包
        立即触发 ``_seq_anomalous`` 又被识别为重连, 形成无限重连循环
        (用户报告: 反复刷"⚡ 检测到同服重连").
        当 ``seq_window > 0`` 时, 只回放与 ``exclude_seq`` 距离不超过
        ``seq_window`` 的包 (双向, 处理 wraparound). 服务器切换 (新 addr)
        不需要过滤, 用 ``seq_window=0`` 表示全量回放.

        返回回放的包数量.
        """
        try:
            candidates = [(s, p) for (a, s, p) in self._recent_pkts
                          if a == addr and s != exclude_seq]
        except Exception:
            return 0
        if seq_window > 0:
            mask = 0xFFFFFFFF
            filtered = []
            for s, p in candidates:
                fwd = (s - exclude_seq) & mask
                bwd = (exclude_seq - s) & mask
                if min(fwd, bwd) <= seq_window:
                    filtered.append((s, p))
            candidates = filtered
        if not candidates:
            # 仍要清理旧 addr 缓存, 否则旧 ISN 包永远留在环形缓冲, 下次真重连
            # 又会被翻出来.
            self._recent_pkts = [(a, s, p) for (a, s, p) in self._recent_pkts
                                 if a != addr]
            return 0
        candidates.sort(key=lambda sp: sp[0])
        replayed = 0
        for s, p in candidates:
            try:
                self._feed_tcp(s, p)
                replayed += 1
            except Exception as e:
                logger.debug(f'[Capture] replay seq={s} failed: {e}')
        if replayed:
            self.stats['replayed_after_change'] += replayed
        self._recent_pkts = [(a, s, p) for (a, s, p) in self._recent_pkts
                             if a != addr]
        return replayed

    # ─── 服务器识别 ───
    def _try_identify(self, data: bytes, addr: str) -> bool:
        """包是否携带任意 c3SB 证据 (含松散方法). 兼容旧调用点.

        v2.3.6: 不再有副作用 — 旧版本会直接 ``self._server_addr = addr``,
        导致跨 addr 调用时即便上层判定逻辑想拒绝, addr 也已经被偷换.
        现在统一返回 bool, 由调用方决定是否切换 ``_server_addr``.
        """
        return self._identify_strict(data) or self._identify_loose(data)

    def _identify_strict(self, data: bytes) -> bool:
        """严格识别: FrameDown(type=6) 嵌套含 c3SB 签名, Login Return,
        或 zstd-wrapped FrameDown.

        v2.3.15: 修复某些地下城场景服务器切换时首包不被识别的问题.
        原版只检查 data[4]==0 && data[5]==6 (非压缩 FrameDown), 但
        某些场景切换首包的 type=0x8006 (zstd 压缩的 FrameDown),
        此时 data[4]=0x80, data[5]=0x06, 旧判断 data[4]==0 失败.
        修复: 提取 msg_type 并检查 & 0x7FFF == 6.
        对于 zstd FrameDown, 嵌套帧被压缩, 无法扫描 c3SB 字面量,
        但 zstd + FrameDown 组合在非游戏流量中几乎不可能出现, 视为
        足够严格.
        """
        # 方法 1: FrameDown (type=6, zstd or plain) 嵌套含 c3SB
        if len(data) > 6:
            _pkt_type_raw = struct.unpack_from('>H', data, 4)[0]
            _is_zstd = bool(_pkt_type_raw & 0x8000)
            _msg_type = _pkt_type_raw & 0x7FFF
            if _msg_type == 6:  # FrameDown
                if _is_zstd:
                    # zstd-wrapped FrameDown: cannot scan c3SB in
                    # compressed data, but zstd+FrameDown combo is
                    # already extremely unlikely in non-game traffic.
                    return True
                # Plain FrameDown: scan nested frames for c3SB
                nested = data[6:]
                if self._scan_c3sb(nested):
                    return True
        # 方法 2: Login Return (0x62 bytes, type=3)
        if (len(data) >= 0x62 and data[:4] == b'\x00\x00\x00\x62'
                and data[4:6] == b'\x00\x03'):
            return True
        return False

    def _identify_loose(self, data: bytes) -> bool:
        """松散识别: payload 中包含 4 字节字面量 ``c3SB``.

        4 字节字面量在大流量下随机命中概率不可忽略 (ZSTD 字典 / 玩家名 /
        buff icon ID 都可能含). 仅用于:
          (1) 初始识别 (``_server_addr is None``) — 此时没有锚点, 必须接受
              松散信号才能完成首次绑定;
          (2) 同服重连判定 — 已经被 v2.3.4 的 ``_seq_anomalous`` (双向
              seq > 1MB) 过滤, 不会被乱序 mid-stream 段误触发.
        **绝不可用于跨 addr 的服务器切换判定**, 否则任何带 c3SB 的非游戏
        TCP 流都会把 ``_server_addr`` 偷换走 → BossHP 反复刷新最后失踪.
        """
        return C3SB_SHORT in data

    def _scan_c3sb(self, data: bytes) -> bool:
        """在嵌套帧数据中扫描 c3SB 签名"""
        if _CY_PACKET is not None:
            try:
                return bool(_CY_PACKET.scan_c3sb_nested(data))
            except Exception:
                pass
        offset = 0
        while offset + 4 < len(data):
            try:
                plen = struct.unpack_from('>I', data, offset)[0]
            except struct.error:
                break
            if plen < 6 or plen > 0xFFFFF:
                break
            end = offset + plen
            if end > len(data):
                break
            payload_start = offset + 4
            payload = data[payload_start:end]
            if len(payload) > 11 and payload[5:5 + 6] == C3SB_SIGNATURE:
                return True
            offset = end
        return False

    # ─── TCP 重组 (参考 C# SRDPS TcpStreamProcessor) ───
    GAP_SKIP_SEC = 2.0  # 缺段等待超时后跳跃 (C# 用 2 秒)

    def _feed_tcp(self, seq: int, data: bytes):
        with self._lock:
            now = time.time()
            self.stats['tcp_segments'] += 1
            # 超时重置
            if self._last_t > 0 and now - self._last_t > 30:
                logger.warning('[Capture] TCP 超时，重置流')
                self._next_seq = -1
                self._cache.clear()
                self._buf = b''
                self._gap_since = 0.0
                self.stats['seq_resets'] += 1

            # 初始化
            if self._next_seq == -1:
                pkt_size = struct.unpack_from('>I', data, 0)[0] if len(data) >= 4 else 0
                # v2.3.9: 与 SRDC / SRDPS 一致, 使用 0x0FFFFF (1048575) 作为
                # 合法帧大小上限. 拥挤地图 SyncNearEntities / SyncContainerData
                # 可超过 1MB, 原先 999999 限制会丢帧.
                if pkt_size < 6 or pkt_size > 0x0FFFFF:
                    return  # 不像有效游戏帧开头
                self._next_seq = seq

            # ── 丢弃已消费的 TCP 重传段 ──
            diff = (seq - self._next_seq) & 0xFFFFFFFF
            if diff > 0x80000000:
                return  # seq 在 _next_seq 之前 (wraparound-safe)

            # 缓存
            self._cache[seq] = data

            # 顺序拼接
            # v2.3.9: 改为本地 list + 一次 join / extend, 避免 busy stream
            # 下 bytes += chunk 触发的 O(n²) 累积复制 (大地图 SyncNearEntities
            # 尤其明显, 会拖慢解析并导致后续段排队丢帧).
            consumed = False
            _new_chunks = []
            while self._next_seq in self._cache:
                chunk = self._cache.pop(self._next_seq)
                _new_chunks.append(chunk)
                self._next_seq = (self._next_seq + len(chunk)) & 0xFFFFFFFF
                self._last_t = now
                consumed = True
            if _new_chunks:
                self._buf = self._buf + b''.join(_new_chunks) if self._buf else b''.join(_new_chunks)

            # ── TCP 缺段跳跃 (参考 C# SRDPS ForceResyncTo) ──
            # 当 pcap 丢失一个段时, _next_seq 卡住, 后续段全进缓存.
            # 等待 GAP_SKIP_SEC 后放弃缺失段, 跳到最低缓存 seq 继续重组.
            if self._cache:
                if consumed:
                    self._gap_since = 0.0
                elif self._gap_since == 0.0:
                    self._gap_since = now
                elif now - self._gap_since >= self.GAP_SKIP_SEC:
                    min_seq = min(self._cache.keys())
                    logger.warning(
                        f'[Capture] TCP gap skip: {len(self._cache)} cached '
                        f'segments, advancing seq'
                    )
                    print(
                        f'[Capture] TCP gap skip: recovering '
                        f'{len(self._cache)} cached segments',
                        flush=True,
                    )
                    self._buf = b''  # 跨间隙的部分帧不可恢复
                    self._next_seq = min_seq
                    self._gap_since = 0.0
                    self.stats['gap_skips'] += 1
                    # 重新消费缓存 (批量 join, 同上避免 O(n²))
                    _skip_chunks = []
                    while self._next_seq in self._cache:
                        chunk = self._cache.pop(self._next_seq)
                        _skip_chunks.append(chunk)
                        self._next_seq = (self._next_seq + len(chunk)) & 0xFFFFFFFF
                        self._last_t = now
                    if _skip_chunks:
                        self._buf = b''.join(_skip_chunks)
            else:
                self._gap_since = 0.0

            # 缓存过大保护
            # v2.3.9: 提高到 2000 段. 大地图拥挤时单次 SyncNearEntities 可
            # 拆成数百 TCP 段, 原先 300 阈值会在 gap skip 尚未触发前就
            # 清空重置, 导致几秒数据全部丢失.
            if len(self._cache) > 2000:
                logger.warning(f'[Capture] TCP cache overflow ({len(self._cache)}), reset')
                self._next_seq = -1
                self._cache.clear()
                self._buf = b''
                self._gap_since = 0.0
                self.stats['cache_overflows'] += 1
                return

        # 提取完整游戏帧 (在锁外做回调)
        self._extract_frames()

    def _extract_frames(self):
        """从 _buf 中切出完整 [4B-size] 帧

        v2.3.9: 使用 offset 指针一次性前进, 一批抽取完所有可用帧, 最后才切片,
        避免 busy map 下每帧一次 self._buf[pkt_size:] 的 O(n) 复制.
        """
        while True:
            frames: list = []
            with self._lock:
                buf = self._buf
                buf_len = len(buf)
                offset = 0
                need_realign = False
                bad_size = 0
                while buf_len - offset >= 6:
                    pkt_size = struct.unpack_from('>I', buf, offset)[0]
                    if pkt_size < 6 or pkt_size > 0x0FFFFF:
                        need_realign = True
                        bad_size = pkt_size
                        break
                    if buf_len - offset < pkt_size:
                        break  # 不够一帧
                    frames.append(bytes(buf[offset:offset + pkt_size]))
                    offset += pkt_size
                # 统一前进一次
                if offset > 0:
                    self._buf = buf[offset:]
                    buf = self._buf
                    buf_len = len(buf)
                if need_realign:
                    # 帧头损坏 — 扫描下一个有效帧头
                    found = False
                    scan_end = min(buf_len - 5, 65536)
                    realign_i = -1
                    if _CY_PACKET is not None:
                        try:
                            realign_i = int(_CY_PACKET.find_frame_realign(buf, 65536))
                        except Exception:
                            realign_i = -1
                    if realign_i > 0:
                        found = True
                    else:
                        _buf_mv = memoryview(buf)
                        try:
                            for i in range(1, scan_end):
                                sz = struct.unpack_from('>I', _buf_mv, i)[0]
                                if 6 <= sz <= 0x0FFFFF:
                                    tp = struct.unpack_from('>H', _buf_mv, i + 4)[0]
                                    msg = tp & 0x7FFF
                                    if msg in (2, 3, 4, 5, 6):
                                        found = True
                                        realign_i = i
                                        break
                        finally:
                            _buf_mv.release()
                    if found:
                        logger.warning(
                            f'[Capture] 帧对齐修复: 跳过 {realign_i} 字节 '
                            f'(bad pkt_size={bad_size})'
                        )
                        self._buf = self._buf[realign_i:]
                    else:
                        logger.error(f'[Capture] 无效帧长度 {bad_size}, 清空流')
                        self._buf = b''
                        self._next_seq = -1
                        self._cache.clear()

            # 回调在锁外执行
            for frame in frames:
                try:
                    self._on_pkt(frame)
                    self.stats['complete_game_frames'] += 1
                except Exception as e:
                    import traceback
                    logger.error(f'[Capture] 帧处理错误: {e}\n{traceback.format_exc()}')

            # 若本轮未取出任何帧且无需重新对齐, 退出循环等待更多数据
            if not frames and not need_realign:
                break


# ═══════════════════════════════════════════════
#  主抓包线程
# ═══════════════════════════════════════════════

class PacketCapture:
    """
    Npcap 抓包主类。
    start() 后在后台线程持续抓包，每收到一个完整游戏帧就调用 on_game_packet 回调。
    """

    def __init__(self, on_game_packet: Callable[[bytes], None],
                 device: Optional[Dict[str, str]] = None,
                 on_server_change: Optional[Callable[[], None]] = None):
        self._on_pkt = on_game_packet
        self._device = device
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._reassembler = TcpReassembler(on_game_packet,
                                            on_server_change=on_server_change)

    @property
    def server_identified(self) -> bool:
        return self._reassembler.server_identified

    @property
    def stats(self) -> Dict[str, int]:
        return dict(getattr(self._reassembler, 'stats', {}) or {})

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True,
                                        name='sao_capture')
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=3)
            self._thread = None

    def _loop(self):
        print(f'[Capture] _loop 线程已启动', flush=True)
        try:
            dll = _load_wpcap()
        except RuntimeError as e:
            print(f'[Capture] wpcap.dll 加载失败: {e}', flush=True)
            logger.error(str(e))
            return

        # 设备选择
        dev = self._device or auto_select_device()
        if not dev:
            print('[Capture] 没有可用网络设备!', flush=True)
            logger.error('[Capture] 没有可用网络设备')
            return

        print(f'[Capture] 设备: {dev["description"]}  name={dev["name"]}', flush=True)
        logger.info(f'[Capture] 使用设备: {dev["description"]}')

        # pcap_open_live
        pcap_open = dll.pcap_open_live
        pcap_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, ctypes.c_char_p]
        pcap_open.restype = ctypes.c_void_p

        pcap_next = dll.pcap_next_ex
        # pcap_next_ex(handle, pkt_header**, pkt_data**) -> int
        pcap_next.argtypes = [ctypes.c_void_p,
                              ctypes.POINTER(ctypes.POINTER(_PcapPkthdr)),
                              ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
        pcap_next.restype = ctypes.c_int

        pcap_close = dll.pcap_close
        pcap_close.argtypes = [ctypes.c_void_p]
        pcap_close.restype = None

        errbuf = ctypes.create_string_buffer(256)
        handle = pcap_open(dev['name'].encode(), 65535, 1, 100, errbuf)
        if not handle:
            err_msg = errbuf.value.decode('utf-8', 'ignore')
            print(f'[Capture] pcap_open_live 失败: {err_msg}', flush=True)
            logger.error(f'[Capture] pcap_open_live 失败: {err_msg}')
            return

        print('[Capture] pcap_open_live 成功, 抓包已启动', flush=True)
        logger.info('[Capture] 抓包已启动')
        pkt_count = 0
        try:
            while self._running:
                hdr_ptr = ctypes.POINTER(_PcapPkthdr)()
                data_ptr = ctypes.POINTER(ctypes.c_ubyte)()
                res = pcap_next(handle, ctypes.byref(hdr_ptr), ctypes.byref(data_ptr))
                if res == 1:
                    caplen = hdr_ptr.contents.caplen
                    raw = ctypes.string_at(data_ptr, caplen)
                    self._reassembler.feed_raw_frame(raw)
                    pkt_count += 1
                    if pkt_count == 1:
                        print(f'[Capture] 首个网络包! caplen={caplen}', flush=True)
                        logger.info('[Capture] 收到首个网络包')
                    if pkt_count <= 3 or pkt_count % 5000 == 0:
                        print(f'[Capture] pkt#{pkt_count} caplen={caplen} reassembler_raw={self._reassembler.stats["raw_frames"]}', flush=True)
                elif res == 0:
                    continue  # 超时
                elif res == -1:
                    print('[Capture] pcap_next_ex 返回 -1 (错误)', flush=True)
                    logger.error('[Capture] pcap_next_ex 错误')
                    break
        except Exception as exc:
            import traceback
            print(f'[Capture] _loop 异常: {exc}\n{traceback.format_exc()}', flush=True)
        finally:
            pcap_close(handle)
            print(f'[Capture] 抓包已停止 (共 {pkt_count} 个包)', flush=True)
            logger.info(f'[Capture] 抓包已停止 (共 {pkt_count} 个包)')


# ═══════════════════════════════════════════════
#  工具
# ═══════════════════════════════════════════════

def _fmt_ip(b: bytes) -> str:
    return '.'.join(str(x) for x in b)
