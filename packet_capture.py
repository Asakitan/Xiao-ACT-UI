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
import ipaddress
import logging
import os
from typing import Optional, Callable, List, Dict, Tuple

logger = logging.getLogger('sao_auto.capture')

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

    # 当前已识别服务器连续多少秒无数据后, 允许重新识别新的场景服务器.
    # 切换地图/副本时, 游戏会断开旧连接并连上新的场景服务器. 若 pcap 缓冲
    # 错过了新服务器的首个 c3SB 登录包, 我们必须在旧服务器静默后释放锁定,
    # 让新服务器的框架包 (game-frame 回退识别) 可以被采用, 否则 DPS /
    # Boss HP 条会一直停留在旧场景的数据上.
    SERVER_IDLE_RESET_SEC = 6.0
    # 回退识别: 某个新地址连续发出多少个疑似游戏帧才视为新场景服务器.
    CANDIDATE_FRAME_THRESHOLD = 3

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

        # 最后一次收到 _server_addr 报文的时间戳. 用于检测旧服务器静默.
        self._last_server_pkt_t: float = 0.0
        # 候选地址 addr → (first_seen_ts, game_frame_count). 当前服务器静默
        # 期间若某地址持续发出疑似游戏帧, 则可以回退识别为新场景服务器.
        self._candidate_addrs: Dict[str, Tuple[float, int]] = {}

        # IP 分片
        self.stats = {
            'raw_frames': 0,
            'tcp_segments': 0,
            'complete_game_frames': 0,
            'seq_resets': 0,
            'cache_overflows': 0,
            'server_changes': 0,
            'idle_reidentify': 0,
            'fallback_identify': 0,
        }
        self._frag = _IpFragmentCache()

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
            self._last_server_pkt_t = 0.0
            self._candidate_addrs.clear()

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

        src_addr = f'{src_ip.hex()}:{sport}'
        server_addr, server_ip, server_port, packet_from_server = \
            self._infer_server_endpoint(src_ip, dst_ip, sport, dport)
        if not server_addr:
            return
        now = time.time()

        # ─── 服务器识别 ───
        if self._server_addr is None:
            if self._try_identify(payload, server_addr):
                logger.info(f'[Capture] 识别到游戏服务器: {_fmt_ip(server_ip)}:{server_port}')
                self._last_server_pkt_t = now
                self._candidate_addrs.clear()
                # 仅服务器→客户端的下行流参与 TCP 重组.
                if packet_from_server:
                    self._feed_tcp(seq, payload)
                return
            # c3SB 未命中: 记作候选. 若同一地址持续发出游戏帧, 则回退识别.
            self._track_candidate(server_addr, payload, now)
            if self._candidate_addrs.get(server_addr, (0, 0))[1] >= self.CANDIDATE_FRAME_THRESHOLD:
                if self._adopt_new_server(payload, server_addr, seq, server_ip, server_port,
                                           reason='initial_game_frame_fallback',
                                           force=True,
                                           feed_current_packet=packet_from_server):
                    self.stats['fallback_identify'] += 1
            return

        # ─── 旧服务器静默 → 允许重新识别 ───
        # 切地图/副本后, 新场景服务器的 c3SB 登录包可能被 pcap 缓冲错过,
        # 导致所有新服务器报文因 addr 不匹配而被丢弃, DPS / Boss HP 冻结.
        # 因此当 _server_addr 连续 N 秒无数据时, 主动释放识别锁.
        if (server_addr != self._server_addr
                and self._last_server_pkt_t > 0
                and now - self._last_server_pkt_t > self.SERVER_IDLE_RESET_SEC):
            old_addr = self._server_addr
            idle = now - self._last_server_pkt_t
            logger.info(
                f'[Capture] 服务器 {old_addr} 空闲 {idle:.1f}s, 释放识别锁'
            )
            print(
                f'[Capture] ⚠ 服务器空闲 {idle:.1f}s, 重新识别新场景服务器',
                flush=True,
            )
            with self._lock:
                self._server_addr = None
                self._next_seq = -1
                self._cache.clear()
                self._buf = b''
            self._last_server_pkt_t = 0.0
            self.stats['idle_reidentify'] += 1
            # 立即尝试以当前包识别新服务器 (优先 c3SB, 退化为 game-frame)
            if self._adopt_new_server(payload, server_addr, seq, server_ip, server_port,
                                       reason='idle_timeout',
                                       treat_as_switch=True,
                                       feed_current_packet=packet_from_server):
                return
            # 未能识别: 记作候选, 等下一个包
            self._track_candidate(server_addr, payload, now)
            return

        if server_addr != self._server_addr:
            # ─── 场景服务器切换检测 ───
            # 切换地图/副本时, 游戏会连接新的场景服务器.
            # 检查来自不同地址的包中是否含有 c3SB 签名,
            # 若有则切换到新服务器. (参考 SRDC: clearDataOnServerChange)
            if self._adopt_new_server(payload, server_addr, seq, server_ip, server_port,
                                       reason='c3SB_switch',
                                       feed_current_packet=packet_from_server):
                return
            # 未命中 c3SB: 记作候选, 若连续出现大量游戏帧则视为新服务器.
            self._track_candidate(server_addr, payload, now)
            if self._candidate_addrs.get(server_addr, (0, 0))[1] >= self.CANDIDATE_FRAME_THRESHOLD:
                if self._adopt_new_server(payload, server_addr, seq, server_ip, server_port,
                                           reason='game_frame_fallback',
                                           force=True,
                                           feed_current_packet=packet_from_server):
                    self.stats['fallback_identify'] += 1
                    return
            return

        # 当前包属于已识别连接, 但方向是客户端 → 服务器; 不能喂给下行 TCP 重组.
        if src_addr != self._server_addr:
            return

        # ─── TCP 重组 ───
        self._last_server_pkt_t = now
        self._feed_tcp(seq, payload)

    # ─── 服务器识别 ───
    def _try_identify(self, data: bytes, addr: str) -> bool:
        """检查包中是否有 c3SB 签名来识别游戏服务器"""
        # 方法 1: FrameDown (type=6)
        if len(data) > 10 and data[4] == 0 and data[5] == 6:
            nested = data[10:]
            if self._scan_c3sb(nested):
                self._server_addr = addr
                return True

        # 方法 2: Login Return (0x62 bytes)
        if (len(data) >= 0x62 and data[:4] == b'\x00\x00\x00\x62'
                and data[4:6] == b'\x00\x03'):
            self._server_addr = addr
            return True

        # 方法 3: 宽松 — 任何包含 c3SB 签名
        if C3SB_SHORT in data:
            self._server_addr = addr
            return True

        return False

    def _scan_c3sb(self, data: bytes) -> bool:
        """在嵌套帧数据中扫描 c3SB 签名"""
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

    @staticmethod
    def _is_private_or_local_ip(ip: bytes) -> bool:
        try:
            addr = ipaddress.ip_address(bytes(ip))
            return bool(
                addr.is_private or addr.is_loopback or addr.is_link_local
                or addr.is_multicast or addr.is_reserved
            )
        except Exception:
            return False

    def _infer_server_endpoint(self, src_ip: bytes, dst_ip: bytes,
                               sport: int, dport: int) -> Tuple[str, bytes, int, bool]:
        """Infer the remote game-server endpoint for this packet.

        Returns (server_addr, server_ip, server_port, packet_from_server).
        The stored _server_addr is always the remote endpoint, which equals the
        source endpoint for inbound packets and the destination endpoint for
        outbound packets.
        """
        src_addr = f'{src_ip.hex()}:{sport}'
        dst_addr = f'{dst_ip.hex()}:{dport}'

        # First, preserve any already identified remote endpoint.
        if self._server_addr == src_addr:
            return src_addr, src_ip, sport, True
        if self._server_addr == dst_addr:
            return dst_addr, dst_ip, dport, False

        src_private = self._is_private_or_local_ip(src_ip)
        dst_private = self._is_private_or_local_ip(dst_ip)
        if src_private != dst_private:
            if not src_private:
                return src_addr, src_ip, sport, True
            return dst_addr, dst_ip, dport, False

        src_ephemeral = int(sport) >= 49152
        dst_ephemeral = int(dport) >= 49152
        if src_ephemeral != dst_ephemeral:
            if not src_ephemeral:
                return src_addr, src_ip, sport, True
            return dst_addr, dst_ip, dport, False

        # Fallback heuristic: the server side usually uses the smaller port.
        if sport != dport:
            if sport < dport:
                return src_addr, src_ip, sport, True
            return dst_addr, dst_ip, dport, False

        return src_addr, src_ip, sport, True

    # ─── 候选地址跟踪 + 回退识别 ───
    @staticmethod
    def _looks_like_game_frame(payload: bytes) -> bool:
        """启发式: 判断 payload 是否以 [4B-size][2B-msg-type] 游戏帧头开始.

        用作 c3SB 登录签名被 pcap 错过时的回退识别手段. 合法的消息类型
        来自 packet_parser.MessageType: NOTIFY=2, RETURN=3, ECHO=4, FRAME_DOWN=6.
        """
        if len(payload) < 6:
            return False
        try:
            size = struct.unpack_from('>I', payload, 0)[0]
            msg_type = struct.unpack_from('>H', payload, 4)[0]
        except struct.error:
            return False
        if size < 6 or size > 0xFFFFF:
            return False
        return msg_type in (2, 3, 4, 6)

    def _track_candidate(self, addr: str, payload: bytes, now: float):
        """记录发出疑似游戏帧的候选地址, 供静默重识别 / 回退识别使用."""
        if not self._looks_like_game_frame(payload):
            return
        first_ts, count = self._candidate_addrs.get(addr, (now, 0))
        self._candidate_addrs[addr] = (first_ts, count + 1)
        # 防止无限增长: 清理 60 秒前的旧候选.
        if len(self._candidate_addrs) > 32:
            self._candidate_addrs = {
                a: v for a, v in self._candidate_addrs.items()
                if now - v[0] < 60.0
            }

    def _adopt_new_server(self, payload: bytes, addr: str, seq: int,
                          server_ip: bytes, server_port: int, reason: str,
                          force: bool = False,
                          treat_as_switch: bool = False,
                          feed_current_packet: bool = True) -> bool:
        """尝试将 addr 采纳为当前游戏服务器地址.

        优先使用 c3SB 签名识别. 若 force=True (例如 game-frame 回退路径), 
        即使没有 c3SB 也会采纳. 返回 True 表示采纳成功, 并已完成状态重置、
        回调触发与首包投喂.

        treat_as_switch=True 时, 即使 _server_addr 已为 None, 也把本次采纳
        当作场景切换 (触发 on_server_change 回调). 用于空闲重识别路径 —
        _server_addr 已被主调清为 None, 但业务上属于切场景.
        """
        old_addr = self._server_addr
        identified = self._try_identify(payload, addr)
        if not identified and not force:
            return False
        self._server_addr = addr
        now = time.time()
        self._last_server_pkt_t = now
        # 重置 TCP 重组状态
        with self._lock:
            self._next_seq = -1
            self._cache.clear()
            self._buf = b''
        self._candidate_addrs.clear()
        is_switch = (old_addr is not None) or treat_as_switch
        if is_switch:
            self.stats['server_changes'] += 1
            logger.info(
                f'[Capture] 场景服务器切换 ({reason}): {old_addr} → {addr} '
                f'({_fmt_ip(server_ip)}:{server_port})'
            )
            print(
                f'[Capture] ⚡ 场景服务器切换 → {_fmt_ip(server_ip)}:{server_port} '
                f'[{reason}] (第 {self.stats["server_changes"]} 次)',
                flush=True,
            )
            if self._on_server_change:
                try:
                    self._on_server_change()
                except Exception as e:
                    logger.error(f'[Capture] on_server_change callback error: {e}')
        else:
            logger.info(
                f'[Capture] 识别到游戏服务器 ({reason}): '
                f'{_fmt_ip(server_ip)}:{server_port}'
            )
            print(
                f'[Capture] 识别到游戏服务器 → {_fmt_ip(server_ip)}:{server_port} '
                f'[{reason}]',
                flush=True,
            )
        # 仅当当前包就是服务器 → 客户端方向时，才把它喂给 TCP 重组。
        if feed_current_packet:
            self._feed_tcp(seq, payload)
        return True

    # ─── TCP 重组 ───
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
                self.stats['seq_resets'] += 1

            # 初始化
            if self._next_seq == -1:
                pkt_size = struct.unpack_from('>I', data, 0)[0] if len(data) >= 4 else 0
                if pkt_size < 6 or pkt_size > 999999:
                    return  # 不像有效游戏帧开头
                self._next_seq = seq

            # 缓存
            self._cache[seq] = data

            # 顺序拼接
            while self._next_seq in self._cache:
                chunk = self._cache.pop(self._next_seq)
                self._buf += chunk
                self._next_seq = (self._next_seq + len(chunk)) & 0xFFFFFFFF
                self._last_t = now

            # 缓存过大保护
            if len(self._cache) > 300:
                logger.warning(f'[Capture] TCP cache overflow ({len(self._cache)}), reset')
                self._next_seq = -1
                self._cache.clear()
                self._buf = b''
                self.stats['cache_overflows'] += 1
                return

        # 提取完整游戏帧 (在锁外做回调)
        self._extract_frames()

    def _extract_frames(self):
        """从 _buf 中切出完整 [4B-size] 帧"""
        while True:
            with self._lock:
                if len(self._buf) < 6:
                    break
                pkt_size = struct.unpack_from('>I', self._buf, 0)[0]
                if pkt_size > 999999:
                    logger.error(f'[Capture] 无效帧长度 {pkt_size}, 清空流')
                    self._buf = b''
                    self._next_seq = -1
                    self._cache.clear()
                    break
                if len(self._buf) < pkt_size:
                    break  # 不够一帧
                frame = self._buf[:pkt_size]
                self._buf = self._buf[pkt_size:]

            # 回调在锁外执行
            try:
                self._on_pkt(frame)
                self.stats['complete_game_frames'] += 1
            except Exception as e:
                import traceback
                logger.error(f'[Capture] 帧处理错误: {e}\n{traceback.format_exc()}')


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
