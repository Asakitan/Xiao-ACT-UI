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
    """自动选择默认网络设备（排除虚拟适配器）"""
    devs = list_devices()
    if not devs:
        return None
    # 排除虚拟适配器
    virtual_keywords = ['vmware', 'virtualbox', 'hyper-v', 'zerotier',
                        'docker', 'wsl', 'vethernet', 'loopback',
                        'npcap loopback', 'bluetooth']
    real_devs = []
    for d in devs:
        desc_low = d['description'].lower()
        if not any(kw in desc_low for kw in virtual_keywords):
            real_devs.append(d)
    return real_devs[0] if real_devs else devs[0]


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
    """

    def __init__(self, on_game_packet: Callable[[bytes], None]):
        self._on_pkt = on_game_packet  # 回调: 一个完整游戏帧
        self._server_addr: Optional[str] = None
        self._lock = threading.Lock()

        # TCP seq 重组
        self._next_seq: int = -1
        self._cache: Dict[int, bytes] = {}
        self._buf = b''
        self._last_t: float = 0

        # IP 分片
        self.stats = {
            'raw_frames': 0,
            'tcp_segments': 0,
            'complete_game_frames': 0,
            'seq_resets': 0,
            'cache_overflows': 0,
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

        # ─── 服务器识别 ───
        if self._server_addr is None:
            if self._try_identify(payload, addr):
                logger.info(f'[Capture] 识别到游戏服务器: {_fmt_ip(src_ip)}:{sport}')
            return

        if addr != self._server_addr:
            return  # 非游戏服务器的包，跳过

        # ─── TCP 重组 ───
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
                logger.error(f'[Capture] 帧处理错误: {e}')


# ═══════════════════════════════════════════════
#  主抓包线程
# ═══════════════════════════════════════════════

class PacketCapture:
    """
    Npcap 抓包主类。
    start() 后在后台线程持续抓包，每收到一个完整游戏帧就调用 on_game_packet 回调。
    """

    def __init__(self, on_game_packet: Callable[[bytes], None],
                 device: Optional[Dict[str, str]] = None):
        self._on_pkt = on_game_packet
        self._device = device
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._reassembler = TcpReassembler(on_game_packet)

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
        try:
            dll = _load_wpcap()
        except RuntimeError as e:
            logger.error(str(e))
            return

        # 设备选择
        dev = self._device or auto_select_device()
        if not dev:
            logger.error('[Capture] 没有可用网络设备')
            return

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
            logger.error(f'[Capture] pcap_open_live 失败: '
                         f'{errbuf.value.decode("utf-8", "ignore")}')
            return

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
                        logger.info('[Capture] 收到首个网络包')
                elif res == 0:
                    continue  # 超时
                elif res == -1:
                    logger.error('[Capture] pcap_next_ex 错误')
                    break
        finally:
            pcap_close(handle)
            logger.info(f'[Capture] 抓包已停止 (共 {pkt_count} 个包)')


# ═══════════════════════════════════════════════
#  工具
# ═══════════════════════════════════════════════

def _fmt_ip(b: bytes) -> str:
    return '.'.join(str(x) for x in b)
