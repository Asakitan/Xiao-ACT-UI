# -*- coding: utf-8 -*-
"""
SAO Auto — 网络抓包数据桥接

PacketBridge 与 RecognitionEngine 接口一致，可直接替换。
通过 Npcap 抓包获取游戏数据，更新到 GameStateManager。

用法:
    from game_state import GameStateManager
    from packet_bridge import PacketBridge

    mgr = GameStateManager()
    bridge = PacketBridge(mgr)
    bridge.start()   # 后台抓包 + 解析 + 推送
    bridge.stop()
"""

import threading
import time
import logging
import math

from game_state import GameStateManager
from packet_parser import PacketParser, PlayerData
from packet_capture import PacketCapture, list_devices, auto_select_device

logger = logging.getLogger('sao_auto.bridge')

# 体力值域追踪 (用于确定 OriginEnergy 含义)
_energy_samples = []          # 最近 N 个 energy 值
_energy_domain = 'unknown'    # 'pct' | 'absolute' | 'unknown'
_stamina_max_cached = 0       # 从 OCR 或观测推断的最大体力


def _sanitize_packet_stamina_max(candidate: int, previous: int) -> int:
    """Reject implausible STA max spikes before they reach the HUD."""
    if candidate <= 0:
        return previous if previous > 0 else 0

    if candidate > 1500:
        if 0 < previous <= 1500:
            return previous
        return 0

    # Allow the first sane self STA cap (for example 1200) to replace
    # placeholder values like 100 coming from the early full-sync path.
    if 0 < previous <= 200 and 500 <= candidate <= 1500:
        return candidate

    if 0 < previous <= 1500:
        if candidate > int(previous * 1.35):
            return previous
        if candidate < int(previous * 0.5):
            return previous

    return candidate


def _resolve_packet_stamina(energy_value: float, stamina_max: int):
    """Convert OriginEnergy into (current, pct) when it looks sane."""
    if stamina_max <= 0:
        return None
    if not isinstance(energy_value, (int, float)):
        return None

    value = float(energy_value)
    if not math.isfinite(value) or value < 0:
        return None

    if value <= 1.05 and stamina_max > 1:
        current = int(round(stamina_max * value))
    else:
        current = int(round(value))

    if current < 0:
        return None
    if current > stamina_max:
        if current <= int(stamina_max * 1.2):
            current = stamina_max
        else:
            return None

    pct = (current / stamina_max) if stamina_max > 0 else 0.0
    return current, pct


class PacketBridge:
    """
    与 RecognitionEngine 同接口的抓包数据桥。

    Interface:
      - __init__(state_mgr, settings=None)
      - start()
      - stop()
      - single_capture() -> Optional[dict]
    """

    def __init__(self, state_mgr: GameStateManager, settings=None):
        self._state_mgr = state_mgr
        self._settings = settings
        self._running = False

        # 抓包层
        self._capture = None
        self._parser = None
        self._thread = None
        self._last_update_t: float = 0
        self._lock = threading.Lock()
        self._stable_sta_current: int = 0
        self._stable_sta_max: int = 0
        self._pending_sta_current = None
        self._pending_sta_hits: int = 0

        # 检查 Npcap 可用性
        self._npcap_ok = False
        self._error_msg = ''

    def start(self):
        """启动抓包，后台线程运行"""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name='sao_bridge')
        self._thread.start()

    def stop(self):
        """停止抓包"""
        self._running = False
        if self._capture:
            try:
                self._capture.stop()
            except Exception:
                pass
        if self._thread:
            self._thread.join(timeout=3)
            self._thread = None

    def single_capture(self):
        """返回当前快照 (兼容 RecognitionEngine 接口)"""
        return self._state_mgr.state.to_dict()

    # ─── 内部 ───

    def _run(self):
        """主运行流程"""
        logger.info('[Bridge] 启动网络抓包数据桥...')

        # ── Npcap 自动安装 ──
        try:
            from install_npcap import ensure_npcap, is_npcap_installed
            if not is_npcap_installed():
                self._state_mgr.update(recognition_ok=False,
                                       error_msg='正在自动安装 Npcap...')
                ok, msg = ensure_npcap(silent=True)
                if not ok:
                    self._error(f'Npcap 安装失败: {msg}')
                    return
        except Exception as e:
            logger.warning(f'[Bridge] Npcap 自动安装检查失败: {e}')

        # 检查设备
        try:
            forced_dev = (self._settings or {}).get('capture_device')
            if forced_dev:
                # 从设备列表里按名称或描述子串匹配
                devs = list_devices()
                dev = next(
                    (d for d in devs
                     if forced_dev in d['name'] or forced_dev in d['description']),
                    None
                )
                if not dev:
                    logger.warning(f'[Bridge] 未找到 capture_device={forced_dev!r}，回退到自动选择')
                    dev = auto_select_device()
            else:
                dev = auto_select_device()
            if not dev:
                self._error('未找到网络设备，请安装 Npcap')
                return
            logger.info(f'[Bridge] 选择网络设备: {dev["description"]}')
        except RuntimeError as e:
            self._error(str(e))
            return

        # 创建解析器
        preferred_uid = 0
        cached_uid = getattr(self._state_mgr.state, 'player_id', '')
        try:
            if isinstance(cached_uid, str) and cached_uid.isdigit():
                preferred_uid = int(cached_uid)
        except Exception:
            preferred_uid = 0
        self._parser = PacketParser(
            on_self_update=self._on_player_update,
            preferred_uid=preferred_uid,
        )

        # 创建抓包器
        self._capture = PacketCapture(
            on_game_packet=self._parser.process_packet,
            device=dev
        )

        self._npcap_ok = True
        self._state_mgr.update(recognition_ok=False,
                               error_msg='等待游戏服务器连接...')

        # 启动抓包
        self._capture.start()

        # 状态监控循环
        while self._running:
            time.sleep(1.0)
            if self._capture.server_identified:
                # 检查数据超时
                with self._lock:
                    if self._last_update_t > 0:
                        idle = time.time() - self._last_update_t
                        if idle > 10:
                            self._state_mgr.update(
                                recognition_ok=False,
                                error_msg=f'数据超时 ({idle:.0f}s)')
                    else:
                        self._state_mgr.update(
                            recognition_ok=False,
                            error_msg='已连接服务器，等待角色数据...')
            else:
                self._state_mgr.update(recognition_ok=False,
                                       error_msg='搜索游戏服务器中...')

        logger.info('[Bridge] 数据桥已停止')

    def _on_player_update(self, player: PlayerData):
        """解析器回调: 当前玩家数据变更"""
        with self._lock:
            self._last_update_t = time.time()

        updates = {
            'recognition_ok': True,
            'error_msg': '',
        }

        if player.name:
            updates['player_name'] = player.name
        if player.uid:
            updates['player_id'] = str(player.uid)
        if player.level > 0:
            updates['level_base'] = player.level
        # Do not treat rank_level as level_extra.
        # Keep the larger recognized extra level to avoid stale packet values
        # overwriting OCR's newer result.
        _season_lv = max(0, player.season_level)
        if _season_lv > 0 and _season_lv >= self._state_mgr.state.level_extra:
            updates['level_extra'] = _season_lv
            logger.info(f'[Bridge] level_extra={_season_lv} (season={player.season_level}, rank={player.rank_level})')
        if player.profession_id > 0:
            updates['profession_id'] = player.profession_id
            if player.profession:
                updates['profession_name'] = player.profession
        if player.max_hp > 0 and player.hp > 0:
            updates['hp_current'] = int(player.hp)
            updates['hp_max'] = int(player.max_hp)
            updates['hp_pct'] = player.hp / player.max_hp
        elif player.max_hp > 0 and player.hp == 0:
            # 只有来自完整同步 (SyncContainerData) 的 HP=0 才接受
            if getattr(player, 'hp_from_full_sync', False):
                updates['hp_current'] = 0
                updates['hp_max'] = int(player.max_hp)
                updates['hp_pct'] = 0.0
                player.hp_from_full_sync = False  # 重置标记
            else:
                logger.debug(f'[Bridge] 忽略增量 HP=0 更新 (max_hp={player.max_hp})')
        elif player.hp == 0 and player.max_hp == 0:
            pass  # 未知，不更新
        else:
            updates['hp_current'] = int(player.hp)

        global _stamina_max_cached
        packet_sta_max = int(
            max(0, getattr(player, 'energy_limit', 0)) +
            max(0, getattr(player, 'extra_energy_limit', 0))
        )
        packet_sta_max = _sanitize_packet_stamina_max(
            packet_sta_max,
            int(self._state_mgr.state.stamina_max or _stamina_max_cached or 0),
        )
        if packet_sta_max > 0:
            _stamina_max_cached = packet_sta_max
        else:
            packet_sta_max = _stamina_max_cached or self._state_mgr.state.stamina_max or 0

        packet_sta = None
        energy_priority = int(getattr(player, 'energy_source_priority', 0) or 0)
        energy_value = getattr(player, 'energy', 0.0)
        if (
            getattr(player, 'energy_valid', False) and
            (
                energy_priority >= 2 or
                (isinstance(energy_value, (int, float)) and 0.0 <= float(energy_value) <= 1.05)
            )
        ):
            packet_sta = _resolve_packet_stamina(player.energy, packet_sta_max)

        if packet_sta is not None:
            sta_cur, sta_pct = packet_sta
            sta_cur = self._stabilize_packet_stamina(sta_cur, packet_sta_max, energy_priority)
            sta_pct = (sta_cur / packet_sta_max) if packet_sta_max > 0 else 0.0
            updates['stamina_current'] = sta_cur
            updates['stamina_max'] = packet_sta_max
            updates['stamina_pct'] = sta_pct
        elif packet_sta_max > 0:
            self._stable_sta_max = packet_sta_max
            updates['stamina_max'] = packet_sta_max

        self._state_mgr.update(**updates)

    def _error(self, msg: str):
        logger.error(f'[Bridge] {msg}')
        self._state_mgr.update(recognition_ok=False, error_msg=msg)

    def _stabilize_packet_stamina(self, current: int, stamina_max: int, energy_priority: int) -> int:
        """Hold packet-only STA spikes until they repeat, instead of showing instant jumps."""
        if stamina_max <= 0:
            self._stable_sta_current = 0
            self._stable_sta_max = 0
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return max(0, int(current))

        current = max(0, min(int(current), int(stamina_max)))
        if self._stable_sta_max != stamina_max:
            self._stable_sta_current = min(max(0, int(self._stable_sta_current or 0)), int(stamina_max))
            self._stable_sta_max = int(stamina_max)
            self._pending_sta_current = None
            self._pending_sta_hits = 0

        if energy_priority != 2:
            self._stable_sta_current = current
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return current

        previous = max(0, min(int(self._stable_sta_current or 0), int(stamina_max)))
        if previous <= 0:
            self._stable_sta_current = current
            return current

        threshold = max(36, int(stamina_max * 0.08))
        if abs(current - previous) <= threshold:
            self._stable_sta_current = current
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return current

        if self._pending_sta_current == current:
            self._pending_sta_hits += 1
        else:
            self._pending_sta_current = current
            self._pending_sta_hits = 1

        if self._pending_sta_hits >= 2:
            logger.info(
                f'[Bridge] accept repeated STA candidate {current}/{stamina_max} '
                f'after filtering spike from {previous}/{stamina_max}'
            )
            self._stable_sta_current = current
            self._pending_sta_current = None
            self._pending_sta_hits = 0
            return current

        logger.debug(
            f'[Bridge] hold STA spike candidate {current}/{stamina_max} '
            f'(prev={previous}/{stamina_max}, priority={energy_priority})'
        )
        return previous
