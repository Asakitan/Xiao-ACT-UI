# -*- coding: utf-8 -*-
"""auto_dodge_director - 定向自动躲避: 把"往某方向撤"翻成相机相对的 WASD 按住。

设计原则 (与活体逆向结论对齐):
- **本地玩家坐标 + 相机前向/右向基** 已活体实证可读 (mem_player_position_reader /
  mem_camera_reader), 是 100% 可靠的躲避基础。
- **相机相对方向** (向后/左/右/斜向) 是最稳的躲避: 打 boss 时面朝它, "向后撤"=脱离
  boss, 直接映射到 W/A/S/D, 连相机都不必读 → 永远正确。
- **世界方向 / 撤向世界坐标点** 需要相机基把世界向量投影到 WASD: dot(d,前向)→W/S,
  dot(d,右向)→D/A。相机基已可读。
- **远程实体(boss)坐标** 远程同步不写本地 lastPosition_, 字段需进本确认 → "远离boss"
  模式留接口但默认降级到相机相对方向 (honest: 不在本里不假装能读 boss 位)。

WASD 同时按住 move_ms 才会真正位移; 可叠加 Shift 连冲 (boss_autokey_linkage 的 dash)。
所有按键经宿主提供的 send_key (与现有引擎同 SendInput), 前台门 + F12 急停由 linkage 兜。
"""
from __future__ import annotations

import math
import threading
import time
from typing import Callable, Dict, List, Optional, Tuple

Vec2 = Tuple[float, float]

# 相机相对方向 → (前向分量, 右向分量), 各 ∈ {-1,0,1}
_CAM_DIRS: Dict[str, Tuple[int, int]] = {
    "forward": (1, 0), "back": (-1, 0), "left": (0, -1), "right": (0, 1),
    "forward_left": (1, -1), "forward_right": (1, 1),
    "back_left": (-1, -1), "back_right": (-1, 1),
}

_DIR_LABELS = {
    "forward": "向前", "back": "向后撤", "left": "向左", "right": "向右",
    "forward_left": "左前", "forward_right": "右前",
    "back_left": "左后撤", "back_right": "右后撤",
    "away_boss": "远离Boss", "away_nearest": "远离最近威胁",
    "goto_point": "走向指定点", "goto_teammate": "靠拢队友(抱团)",
    "goto_circle": "走向编号圈", "walk_sequence": "按序走完编号圈",
}

# 走向类方向 (move TOWARD): 闭环走位, 与 away_*(远离)相反
_GOTO_DIRECTIONS = ("goto_point", "goto_teammate", "goto_circle", "walk_sequence")


def direction_label(d: str) -> str:
    return _DIR_LABELS.get(d, d or "")


def _axis_keys(fwd: int, right: int) -> List[str]:
    keys: List[str] = []
    if fwd > 0:
        keys.append("W")
    elif fwd < 0:
        keys.append("S")
    if right > 0:
        keys.append("D")
    elif right < 0:
        keys.append("A")
    return keys


def world_vec_to_keys(world_dx: float, world_dz: float,
                      cam_forward: Vec2, cam_right: Vec2,
                      thresh: float = 0.35) -> List[str]:
    """把世界 XZ 方向向量投影到相机基, 给出要按住的 WASD 键集。"""
    m = math.hypot(world_dx, world_dz)
    if m < 1e-5:
        return []
    dx, dz = world_dx / m, world_dz / m
    fwd_c = dx * cam_forward[0] + dz * cam_forward[1]
    rgt_c = dx * cam_right[0] + dz * cam_right[1]
    return _axis_keys(1 if fwd_c > thresh else (-1 if fwd_c < -thresh else 0),
                      1 if rgt_c > thresh else (-1 if rgt_c < -thresh else 0))


def resolve_dodge_keys(spec: Dict, *,
                       cam_basis: Optional[Dict] = None,
                       player_pos: Optional[Tuple[float, float, float]] = None,
                       danger_pos: Optional[Tuple[float, float, float]] = None
                       ) -> Tuple[List[str], str]:
    """根据 dodge spec 解出要按住的 WASD 键集 + 人类可读说明。

    spec.direction:
      ''                 不定向 (只走原有 key/sequence)
      camera 相对         forward/back/left/right/forward_left/.../back_right
      world:<dx>,<dz>     固定世界方向 (需 cam_basis)
      away_point:<x>,<z>  远离某世界坐标点 (需 cam_basis + player_pos)
      away_boss           远离危险源 (需 cam_basis + player_pos + danger_pos; 缺则降级)
    """
    direction = str(spec.get("direction") or "").strip()
    if not direction:
        return [], ""
    if direction in _CAM_DIRS:                       # 相机相对: 最稳, 不必读相机
        fwd, right = _CAM_DIRS[direction]
        return _axis_keys(fwd, right), direction_label(direction)
    fwd_v = cam_basis.get("forward") if cam_basis else None
    rgt_v = cam_basis.get("right") if cam_basis else None
    fallback = str(spec.get("fallback_direction") or "back")

    def _fallback(reason: str):
        fwd, right = _CAM_DIRS.get(fallback, (-1, 0))
        return _axis_keys(fwd, right), "%s(降级:%s)" % (direction_label(fallback), reason)

    if not fwd_v or not rgt_v:
        return _fallback("无相机")
    if direction.startswith("world:"):
        try:
            dx, dz = [float(x) for x in direction[6:].split(",")[:2]]
        except Exception:
            return _fallback("方向解析失败")
        return world_vec_to_keys(dx, dz, fwd_v, rgt_v), "撤向世界方向"
    if direction.startswith("away_point:"):
        if not player_pos:
            return _fallback("无玩家位")
        try:
            px, pz = [float(x) for x in direction[11:].split(",")[:2]]
        except Exception:
            return _fallback("坐标解析失败")
        return (world_vec_to_keys(player_pos[0] - px, player_pos[2] - pz,
                                  fwd_v, rgt_v), "远离指定点")
    if direction in ("away_boss", "away_nearest"):
        # 两者数学相同, 危险源由宿主 (boss位 / 最近威胁位) 注入 danger_pos
        if player_pos and danger_pos:
            return (world_vec_to_keys(player_pos[0] - danger_pos[0],
                                      player_pos[2] - danger_pos[2],
                                      fwd_v, rgt_v),
                    "远离最近威胁" if direction == "away_nearest" else "远离危险源")
        return _fallback("危险位未知")
    return _fallback("未知方向")


class AutoDodgeDirector:
    """把定向躲避 spec 落成实际按键 (按住 WASD move_ms + 可选 Shift 连冲)。

    宿主注入:
      key_event(key, down)                   # 低层按下(down=True)/松开(down=False)
      get_cam_basis() -> dict|None           # 相机前向/右向 (世界 XZ)
      get_player_pos() -> (x,y,z)|None
      gate() -> bool                         # 前台门 (False=不发); 派发中复查可急停
    """

    def __init__(self, key_event: Callable, *,
                 get_cam_basis: Optional[Callable] = None,
                 get_player_pos: Optional[Callable] = None,
                 gate: Optional[Callable] = None):
        self._key_event = key_event
        self._cam = get_cam_basis
        self._pos = get_player_pos
        self._gate = gate
        self._held: List[str] = []
        self._lock = threading.Lock()
        self._epoch = 0          # 单飞: 每次新 dodge +1, 旧循环检测到不等即退出

    def _blocked(self) -> bool:
        if self._gate is None:
            return False
        try:
            return not bool(self._gate())
        except Exception:
            return False

    def _press(self, key: str, up: bool) -> None:
        # 真·按下/松开 (移动键需同时按住, 单键 tap/hold 做不到对角移动)
        try:
            self._key_event(key, not up)
        except Exception:
            pass

    def _next_epoch(self) -> int:
        with self._lock:
            self._epoch += 1
            return self._epoch

    def dodge(self, spec: Dict, *, danger_pos=None, get_danger_pos=None,
             is_clear=None) -> Dict:
        """执行一次定向躲避。返回 {keys, label, fired}。非定向 spec 直接 no-op。

        get_danger_pos (可选): 危险源世界坐标的 O(1) 读回调 (宿主已锁定 obj)。提供且方向
        为 away_* 时走**闭环精准出圈**——持续读玩家/危险位重算 WASD。
        is_clear (可选): 零误差出圈判据回调——游戏自己的区域成员判定(玩家离开 AOE 圈即
        True), 优先于 exit_margin 距离; 提供时距离仅作兜底。"""
        cam = self._cam_safe()
        pos = self._pos_safe()
        keys, label = resolve_dodge_keys(spec, cam_basis=cam, player_pos=pos,
                                         danger_pos=danger_pos)
        if not keys:
            return {"keys": [], "label": label, "fired": False}
        epoch = self._next_epoch()
        move_ms = max(80, min(6000, int(spec.get("move_ms") or 600)))
        direction = str(spec.get("direction") or "")
        if get_danger_pos is not None and direction.startswith("away"):
            safe_dist = float(spec.get("exit_margin_m") or 0) or 6.0
            fb = list(_CAM_DIRS.get(str(spec.get("fallback_direction") or "back"),
                                    (-1, 0)))
            threading.Thread(target=self._run_until_clear,
                             args=(epoch, spec, get_danger_pos, safe_dist, move_ms,
                                   _axis_keys(fb[0], fb[1]), is_clear),
                             daemon=True).start()
            tag = "精准出圈(区域判定)" if is_clear is not None else "精准出圈"
            return {"keys": keys, "label": "%s·%s" % (label, tag), "fired": True}
        threading.Thread(target=self._run_move, args=(epoch, keys, move_ms),
                         daemon=True).start()
        return {"keys": keys, "label": label, "fired": True}

    def walk_to(self, get_target_pos: Callable, *, arrive_m: float = 2.0,
                max_ms: int = 8000, is_arrived: Optional[Callable] = None,
                label: str = "自动走位") -> Dict:
        """闭环走向一个世界坐标点 (与 dodge 的"远离"相反): 每 tick 读玩家位+目标位+相机
        基 → 朝目标的 WASD, 水平距 ≤ arrive_m 或 is_arrived() 即停。

        ★安全: 走位读不到位置/相机/目标时**直接停, 绝不盲按键**(盲走可能走进危险);
        这是与躲避(可降级纯按键后撤)的关键区别。get_target_pos 返回 None → 该 tick 停。"""
        epoch = self._next_epoch()
        max_ms = max(200, min(15000, int(max_ms)))
        arrive_m = max(0.5, float(arrive_m))
        threading.Thread(target=self._run_until_arrive,
                         args=(epoch, get_target_pos, arrive_m, max_ms, is_arrived),
                         daemon=True).start()
        return {"label": label, "fired": True, "epoch": epoch}

    def _run_until_arrive(self, epoch: int, get_target_pos: Callable,
                          arrive_m: float, max_ms: int,
                          is_arrived: Optional[Callable]) -> None:
        tick = 0.07
        t0 = time.time()
        try:
            while (time.time() - t0) < max_ms / 1000.0:
                if self._blocked() or epoch != self._epoch:
                    break
                if is_arrived is not None:
                    try:
                        if is_arrived():
                            break              # 宿主判定已到位(如区域成员=已进圈)
                    except Exception:
                        pass
                pp = self._pos_safe()
                tp = None
                try:
                    tp = get_target_pos()
                except Exception:
                    tp = None
                cam = self._cam_safe()
                if not pp or not tp or not cam:
                    break                      # ★读不到 → 停, 不盲走
                if is_arrived is None and \
                        math.hypot(pp[0] - tp[0], pp[2] - tp[2]) <= arrive_m:
                    break                      # 简单走点: 到半径即停 (序列走由 is_arrived 推进)
                keys = world_vec_to_keys(tp[0] - pp[0], tp[2] - pp[2],
                                         cam["forward"], cam["right"])
                if not keys:
                    break
                self._apply_keys(keys)
                time.sleep(tick)
        finally:
            self._release_if_mine(epoch)

    def _cam_safe(self):
        try:
            return self._cam() if self._cam else None
        except Exception:
            return None

    def _pos_safe(self):
        try:
            return self._pos() if self._pos else None
        except Exception:
            return None

    def _apply_keys(self, target: List[str]) -> None:
        """把 _held 调整到 target 键集 (只对变化的键 press/release, 不抖)。"""
        with self._lock:
            cur = set(self._held)
            want = set(target)
            for k in cur - want:
                self._press(k, up=True)
            for k in want - cur:
                self._press(k, up=False)
            self._held = list(want)

    def _run_move(self, epoch: int, keys: List[str], move_ms: int) -> None:
        if self._blocked() or epoch != self._epoch:
            return
        self._apply_keys(keys)
        try:
            step = 0.05
            waited = 0.0
            while waited < move_ms / 1000.0:
                if self._blocked() or epoch != self._epoch:
                    break
                time.sleep(step)
                waited += step
        finally:
            self._release_if_mine(epoch)

    def _run_until_clear(self, epoch: int, spec: Dict, get_danger_pos: Callable,
                         safe_dist: float, max_ms: int, fallback_keys: List[str],
                         is_clear: Optional[Callable] = None) -> None:
        """闭环: 持续把人物往远离危险源方向挪, 出圈即停 (精准出圈)。
        停止判据优先级: (1) is_clear() 区域成员判定=零误差(玩家离开AOE圈) > (2) 水平距
        ≥safe_dist 距离兜底。热路径 O(1): 每 tick 只读玩家位+危险位+相机基(缓存);
        任一读不到→降级到相机相对 fallback 纯按键(不再读内存)。"""
        import math
        tick = 0.07
        t0 = time.time()
        try:
            while (time.time() - t0) < max_ms / 1000.0:
                if self._blocked() or epoch != self._epoch:
                    break
                if is_clear is not None:
                    try:
                        if is_clear():
                            break                      # 零误差: 玩家已离开 AOE 圈
                    except Exception:
                        pass
                pp = self._pos_safe()
                dp = None
                try:
                    dp = get_danger_pos()
                except Exception:
                    dp = None
                cam = self._cam_safe()
                if not pp or not dp or not cam:
                    self._apply_keys(fallback_keys)   # 降级: 不读内存的纯按键后撤
                    time.sleep(tick)
                    continue
                if is_clear is None and \
                        math.hypot(pp[0] - dp[0], pp[2] - dp[2]) >= safe_dist:
                    break                              # 距离兜底(无区域判定时)
                keys = world_vec_to_keys(pp[0] - dp[0], pp[2] - dp[2],
                                         cam["forward"], cam["right"])
                if not keys:
                    keys = fallback_keys
                self._apply_keys(keys)
                time.sleep(tick)
        finally:
            self._release_if_mine(epoch)

    def _release_if_mine(self, epoch: int) -> None:
        with self._lock:
            if epoch != self._epoch:
                return        # 已被新 dodge 接管, 别松别人按的键
            for k in list(self._held):
                self._press(k, up=True)
            self._held = []

    def release_all(self) -> None:
        """急停: 松开所有按住的移动键并作废在途循环 (F12 / panic 调用)。"""
        with self._lock:
            self._epoch += 1      # 作废所有在飞循环
            for k in list(self._held):
                self._press(k, up=True)
            self._held = []


__all__ = ["AutoDodgeDirector", "resolve_dodge_keys", "world_vec_to_keys",
           "direction_label"]
