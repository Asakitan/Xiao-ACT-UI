"""TCP-驱动的全自动锚点定位.

不需要手动输入数值: 后台启动 PacketBridge 抓包, 读取 GameStateManager
和 parser._current_uid 作为 ground truth, 自动驱动 scanner 收敛。

用法 (管理员 PowerShell):
    cd e:\\VC\\SAO-UI\\sao_auto
    e:\\Py\\python.exe -m tools.mem_probe.auto_locate

需求:
    - 已安装 Npcap (主程序若能正常抓包就 OK)
    - Star.exe 在线且已登录到主城/任意场景 (HP > 0)
    - 整个过程让游戏内 HP 自然变化几次 (走两步、挨一下、吃个药都行),
      30~60 秒应该能收敛 self UID + HP + MaxHP 三个锚点

输出:
    tools/mem_probe/anchors.json  (基于游戏当前 PID 写入, 不跨重启复用)
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from typing import Any, Dict, List, Optional

# 让 sao_auto/ 在 sys.path 顶层, 便于 import game_state / packet_bridge
_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from .process import StarProcess, StarProcessError, is_admin
from .scanner import narrow, scan


# ───────────────────────── TCP 数据源 ─────────────────────────
class _TcpSource:
    """轻量包装: 启动 PacketBridge, 暴露 self UID / HP / MaxHP / Name."""

    def __init__(self) -> None:
        from game_state import GameStateManager
        from packet_bridge import PacketBridge

        self._state_mgr = GameStateManager()
        # 不传 settings/callbacks: 我们只需要它把状态写进 state_mgr
        self._bridge = PacketBridge(self._state_mgr)
        self._bridge.start()

    def stop(self) -> None:
        try:
            self._bridge.stop()
        except Exception:
            pass

    def snapshot(self) -> Dict[str, Any]:
        st = self._state_mgr.state
        parser = getattr(self._bridge, "_parser", None)
        uid = 0
        try:
            if parser is not None:
                uid = int(getattr(parser, "_current_uid", 0) or 0)
        except Exception:
            uid = 0
        return {
            "uid": uid,
            "name": str(getattr(st, "player_name", "") or ""),
            "hp": int(getattr(st, "hp_current", 0) or 0),
            "max_hp": int(getattr(st, "hp_max", 0) or 0),
            "in_combat": bool(getattr(st, "in_combat", False)),
            "packet_active": bool(getattr(st, "packet_active", False)),
        }

    def wait_ready(self, *, timeout: float = 60.0, interval: float = 0.5) -> Dict[str, Any]:
        """阻塞等待 self UID + HP > 0; 超时抛 RuntimeError."""
        deadline = time.time() + timeout
        last_print = 0.0
        while time.time() < deadline:
            snap = self.snapshot()
            if snap["uid"] and snap["hp"] > 0:
                return snap
            now = time.time()
            if now - last_print > 2.0:
                last_print = now
                print(
                    f"[wait] packet_active={snap['packet_active']} "
                    f"uid={snap['uid']} hp={snap['hp']}/{snap['max_hp']} "
                    f"name={snap['name']!r}"
                )
            time.sleep(interval)
        raise RuntimeError(
            f"等待 TCP 数据超时 ({timeout}s); 最后快照: {self.snapshot()}"
        )


# ───────────────────────── 地址格式化 ─────────────────────────
def _fmt_addr(pm: StarProcess, addr: int, mods=None) -> str:
    if mods is None:
        try:
            mods = pm.list_modules()
        except Exception:
            mods = []
    for m in mods:
        if m.base <= addr < m.base + m.size:
            return f"0x{addr:016X} ({m.name}+0x{addr - m.base:X})"
    return f"0x{addr:016X} (heap)"


def _hex_context(pm: StarProcess, addr: int, before: int = 16, after: int = 48) -> str:
    start = max(0, addr - before)
    n = before + after
    data = pm.read_bytes(start, n)
    if data is None:
        return "  (read failed)"
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        marker = " <--" if start + i <= addr < start + i + 16 else ""
        out.append(f"  {start + i:016X}  {hex_part:<48}  {ascii_part}{marker}")
    return "\n".join(out)


# ───────────────────────── 阶段实现 ─────────────────────────
def locate_uid(pm: StarProcess, uid: int) -> List[int]:
    """UID 通常全局唯一, 一次扫描即可定位."""
    print(f"[uid] scanning i64 = 0x{uid & 0xFFFFFFFFFFFFFFFF:016X} ({uid})...")
    t0 = time.time()
    hits = scan(pm, uid, "i64")
    print(f"[uid] {len(hits)} candidates in {time.time()-t0:.2f}s")
    return hits


def locate_hp(
    pm: StarProcess,
    src: _TcpSource,
    *,
    timeout: float = 90.0,
    poll: float = 0.2,
    target_count: int = 3,
) -> List[int]:
    """轮询 TCP HP, 当数值变化时自动 narrow.

    target_count: 收敛到 <= 此数即停止 (默认 3, 对 PoC 已足够)
    """
    snap = src.snapshot()
    cur = snap["hp"]
    print(f"[hp] initial scan i32 = {cur} (max={snap['max_hp']})")
    t0 = time.time()
    candidates = scan(pm, cur, "i32", max_hits=300_000)
    print(f"[hp] frame 0: {len(candidates)} candidates in {time.time()-t0:.2f}s")
    if len(candidates) <= target_count:
        return candidates

    print(f"[hp] 请在游戏内让 HP 变动 (走两步/挨一下/吃药), 等待自动收敛...")
    last_value = cur
    deadline = time.time() + timeout
    frame = 0
    while time.time() < deadline:
        time.sleep(poll)
        new_val = src.snapshot()["hp"]
        if new_val == last_value or new_val <= 0:
            continue
        frame += 1
        t0 = time.time()
        candidates = narrow(pm, candidates, new_val, "i32")
        print(
            f"[hp] frame {frame}: {last_value} -> {new_val}, "
            f"narrowed to {len(candidates)} in {(time.time()-t0)*1000:.0f}ms"
        )
        last_value = new_val
        if len(candidates) <= target_count:
            return candidates
        if len(candidates) == 0:
            print("[hp] candidate set EMPTY -> 重置首扫")
            t0 = time.time()
            candidates = scan(pm, new_val, "i32", max_hits=300_000)
            print(f"[hp] re-scan: {len(candidates)} candidates in {time.time()-t0:.2f}s")
    print(f"[hp] timeout ({timeout}s); leaving {len(candidates)} candidates")
    return candidates


def locate_max_hp(
    pm: StarProcess,
    max_hp: int,
    *,
    near_hits: Optional[List[int]] = None,
    near_radius: int = 0x100,
) -> List[int]:
    """MaxHP 一般不变; 单帧扫描后, 如果给了 hp 候选, 就用'相邻性'过滤."""
    print(f"[maxhp] scanning i32 = {max_hp}")
    t0 = time.time()
    cands = scan(pm, max_hp, "i32", max_hits=300_000)
    print(f"[maxhp] {len(cands)} candidates in {time.time()-t0:.2f}s")
    if not near_hits or not cands:
        return cands
    # near_hits 按地址排序, 用二分加速
    sorted_near = sorted(near_hits)
    import bisect

    filtered = []
    for a in cands:
        i = bisect.bisect_left(sorted_near, a)
        for j in (i - 1, i):
            if 0 <= j < len(sorted_near):
                if abs(sorted_near[j] - a) <= near_radius:
                    filtered.append(a)
                    break
    print(
        f"[maxhp] {len(filtered)} 候选距离某个 HP 候选 <= 0x{near_radius:X}"
    )
    return filtered


# ───────────────────────── 主流程 ─────────────────────────
def run(
    *,
    tcp_timeout: float = 60.0,
    hp_timeout: float = 90.0,
    out_path: Optional[str] = None,
) -> int:
    if not is_admin():
        print("[warn] 未以管理员身份运行, OpenProcess 可能失败。", file=sys.stderr)

    print("[boot] starting PacketBridge in background...")
    src = _TcpSource()
    try:
        ready = src.wait_ready(timeout=tcp_timeout)
        print(
            f"[boot] TCP ready: uid={ready['uid']} hp={ready['hp']}/{ready['max_hp']} "
            f"name={ready['name']!r}"
        )

        try:
            pm = StarProcess()
        except StarProcessError as e:
            print(f"[fail] {e}", file=sys.stderr)
            return 2
        try:
            mods = pm.list_modules()
            print(f"[mem] attached pid={pm.pid}, {len(mods)} modules")

            # ─── 阶段 A: UID ───
            uid_hits = locate_uid(pm, ready["uid"])

            # ─── 阶段 B: HP (动态收敛) ───
            hp_hits = locate_hp(pm, src, timeout=hp_timeout)

            # ─── 阶段 C: MaxHP (与 HP 邻近过滤) ───
            mh_hits: List[int] = []
            current = src.snapshot()
            if current["max_hp"] > 0:
                mh_hits = locate_max_hp(pm, current["max_hp"], near_hits=hp_hits)

            # ─── 报告 ───
            print("\n" + "=" * 72)
            print("ANCHOR REPORT")
            print("=" * 72)
            for label, hits in (
                ("self_uid (i64)", uid_hits),
                ("self_hp (i32)", hp_hits),
                ("self_max_hp (i32, near hp)", mh_hits),
            ):
                print(f"\n[{label}] {len(hits)} candidate(s):")
                for a in hits[:10]:
                    print(f"   {_fmt_addr(pm, a, mods)}")
                if len(hits) > 10:
                    print(f"   ... ({len(hits) - 10} more)")
                if 1 <= len(hits) <= 3:
                    for a in hits:
                        print(f"\n   context @ {_fmt_addr(pm, a, mods)}:")
                        print(_hex_context(pm, a))

            # ─── 持久化 ───
            if out_path is None:
                out_path = os.path.join(
                    os.path.dirname(os.path.abspath(__file__)), "anchors.json"
                )
            payload = {
                "saved_at": time.time(),
                "pid": pm.pid,
                "ground_truth": ready,
                "uid_candidates": [hex(a) for a in uid_hits],
                "hp_candidates": [hex(a) for a in hp_hits],
                "max_hp_candidates": [hex(a) for a in mh_hits],
            }
            with open(out_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, indent=2, ensure_ascii=False)
            print(f"\n[ok] anchors saved -> {out_path}")
            return 0
        finally:
            pm.close()
    finally:
        src.stop()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.auto_locate")
    p.add_argument("--tcp-timeout", type=float, default=60.0,
                   help="等待 TCP 数据就绪的超时 (秒, 默认 60)")
    p.add_argument("--hp-timeout", type=float, default=90.0,
                   help="HP 自动收敛的超时 (秒, 默认 90)")
    p.add_argument("--out", default=None, help="anchors.json 输出路径")
    args = p.parse_args(argv)
    try:
        return run(
            tcp_timeout=args.tcp_timeout,
            hp_timeout=args.hp_timeout,
            out_path=args.out,
        )
    except KeyboardInterrupt:
        print("\n[abort] user interrupted")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
