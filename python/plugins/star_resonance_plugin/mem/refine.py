# anchors.json 二次精化.
#
# 前置: 先跑 auto_locate.py 拿到 anchors.json (含 uid/hp/max_hp 候选集)。
#
# 工作流程:
# 1. 复用 anchors.json + 重新 attach Star.exe
# 2. 启动 PacketBridge 监控 HP 实时变化
# 3. 提示玩家"现在请挨一下小怪让 HP 真正下降" (满血时 HP==MaxHP 无法区分)
# 4. HP 下降后:
# - 用新 HP narrow → 真 self_hp 地址集 (期望 1~3)
# - MaxHP 集仍按旧值, 用 narrow 验证 (大概率不变)
# - 求 hp 候选与 maxhp 候选的"邻接对" (offset ±4/±8/±0x10/±0x18 等典型布局)
# - 对每个邻接对, 在其 ±0x800 范围内找 uid 候选 → 唯一锁定 Player struct
#
# 输出:
# 覆盖 anchors.json, 新增字段:
# - self_hp_addr: int (single)
# - self_max_hp_addr: int (single)
# - self_uid_addr: int (single, 在 Player struct 内)
# - player_struct_base_guess: int (推测 hp_addr 所在的对象起点)

from __future__ import annotations

import argparse
import bisect
import json
import os
import sys
import time
from typing import Dict, List, Optional, Tuple

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from .auto_locate import _TcpSource, _fmt_addr, _hex_context
from .process import StarProcess, StarProcessError, is_admin
from .scanner import narrow

DEFAULT_ANCHORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")

# Player 对象内 hp/maxhp 的常见相对偏移 (典型 C# class 字段紧挨)
_HP_MAXHP_DELTAS = (4, 8, 0x10, 0x18, 0x20, -4, -8, -0x10, -0x18, -0x20)
_UID_NEAR_RADIUS = 0x800  # uid 与 hp 同对象时通常在 ±2 KB 内


def _load_anchors(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _save_anchors(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def _wait_any_hp_change(src: _TcpSource, *, baseline: int, timeout: float):
    # 等到至少看到一个与 baseline 不同的 HP. 返回 (changed_hp, latest_max_hp).
    print(f"[wait] baseline HP={baseline}")
    print("[wait] >>> 请在游戏内让 HP 变动 (挨打/吃药/施放消耗都行) <<<")
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.2)
        snap = src.snapshot()
        cur = snap["hp"]
        if cur > 0 and cur != baseline:
            return cur, snap["max_hp"]
    raise RuntimeError(f"timeout waiting for HP change; baseline={baseline}")


def _verify_lockstep(
    pm,
    src: _TcpSource,
    candidates: List[int],
    *,
    field: str = "hp",
    samples: int = 25,
    interval: float = 0.1,
    min_match_ratio: float = 0.6,
) -> List[Tuple[int, int]]:
    # 对每个 candidate 在 N 次 TCP 采样的同一瞬间读内存, 计 read==tcp 命中数.
    #
    # HP 抖动场景下, 真正 self_hp 地址会与 TCP 完全同步; 只要内存读和 TCP 报到一致,
    # 就算几乎所有时刻 HP 都在变, 真地址也会高分。
    #
    # 返回 [(addr, score)] 按 score 倒序; 只保留 score >= samples * min_match_ratio。
    if not candidates:
        return []
    score = {a: 0 for a in candidates}
    valid = 0
    for k in range(samples):
        snap = src.snapshot()
        tcp = snap[field]
        if tcp is None or tcp <= 0:
            time.sleep(interval)
            continue
        valid += 1
        for a in candidates:
            b = pm.read_bytes(a, 4)
            if b is None:
                continue
            v = int.from_bytes(b, "little", signed=True)
            if v == tcp:
                score[a] += 1
        time.sleep(interval)
    if valid == 0:
        return []
    threshold = max(2, int(valid * min_match_ratio))
    survivors = [(a, s) for a, s in score.items() if s >= threshold]
    survivors.sort(key=lambda x: -x[1])
    print(f"[lockstep] {len(candidates)} candidates × {valid} samples → "
          f"{len(survivors)} pass (threshold={threshold}/{valid})")
    return survivors


def _multi_scan_hp(
    pm,
    src: _TcpSource,
    *,
    snapshot_window: float = 3.0,
    snapshot_interval: float = 0.15,
) -> List[int]:
    # 在 snapshot_window 秒内收集若干个不同 HP 值, 各扫一次取并集.
    #
    # HP 一直跳的角色, 单次扫描扫到一半 HP 就变了 → 命中数极少。
    # 多 HP 并集能保证真实 hp_addr 一定在结果集中 (它必然在某个时刻等于其中之一)。
    from .scanner import scan as _scan
    deadline = time.time() + snapshot_window
    seen: List[int] = []
    while time.time() < deadline:
        h = src.snapshot()["hp"]
        if h > 0 and h not in seen:
            seen.append(h)
        time.sleep(snapshot_interval)
    if not seen:
        return []
    print(f"[multi-scan] 收集到 {len(seen)} 个不同 HP 快照: {seen[:5]}{'...' if len(seen)>5 else ''}")
    union: set = set()
    for v in seen:
        t0 = time.time()
        hits = _scan(pm, v, "i32", max_hits=300_000)
        print(f"   scan hp={v}: {len(hits)} hits ({time.time()-t0:.1f}s)")
        union.update(hits)
        if len(union) > 500_000:
            print(f"   union 已达 {len(union)}, 停止以避免内存爆")
            break
    out = sorted(union)
    print(f"[multi-scan] 并集: {len(out)} 个候选地址")
    return out


def _stream_narrow(
    pm,
    src: _TcpSource,
    initial: List[int],
    *,
    target_count: int = 3,
    timeout: float = 120.0,
    poll: float = 0.2,
) -> List[int]:
    # 持续监听 TCP HP, 每次新值出现就 narrow; 候选 ≤ target_count 立即返回.
    #
    # 第一帧无论 candidates 数量都先做一次基线 narrow 来剔除 unreadable 地址,
    # 避免上次会话的陈旧地址被误当成 "already converged".
    candidates = list(initial)
    cur0 = src.snapshot()["hp"]
    if cur0 > 0 and candidates:
        t0 = time.time()
        candidates = narrow(pm, candidates, cur0, "i32")
        print(
            f"[narrow] baseline: HP={cur0}, "
            f"candidates {len(initial)} -> {len(candidates)} ({(time.time()-t0)*1000:.0f}ms)"
        )
        if not candidates:
            print("[narrow] baseline 后候选已空 (旧地址全失效); 触发上层重扫")
            return []
    last_value = cur0
    deadline = time.time() + timeout
    frame = 0
    while time.time() < deadline and len(candidates) > target_count:
        time.sleep(poll)
        cur = src.snapshot()["hp"]
        if cur <= 0 or cur == last_value:
            continue
        frame += 1
        t0 = time.time()
        candidates = narrow(pm, candidates, cur, "i32")
        print(
            f"[narrow] frame {frame}: HP {last_value} -> {cur}, "
            f"candidates -> {len(candidates)} ({(time.time()-t0)*1000:.0f}ms)"
        )
        last_value = cur
        if len(candidates) == 0:
            print("[narrow] candidate set EMPTY -> 该地址可能不直接存 HP, 终止")
            break
    return candidates


def _find_pairs(
    hp_addrs: List[int], maxhp_addrs: List[int]
) -> List[Tuple[int, int, int]]:
    # 返回 [(hp_addr, maxhp_addr, delta)], delta = maxhp - hp.
    sorted_max = sorted(maxhp_addrs)
    pairs: List[Tuple[int, int, int]] = []
    for h in hp_addrs:
        for d in _HP_MAXHP_DELTAS:
            target = h + d
            i = bisect.bisect_left(sorted_max, target)
            if i < len(sorted_max) and sorted_max[i] == target:
                pairs.append((h, target, d))
    return pairs


def _find_nearby_uid(
    uid_addrs: List[int], anchor: int, radius: int = _UID_NEAR_RADIUS
) -> List[int]:
    sorted_u = sorted(uid_addrs)
    lo = bisect.bisect_left(sorted_u, anchor - radius)
    hi = bisect.bisect_right(sorted_u, anchor + radius)
    return sorted_u[lo:hi]


def _local_find_i32(
    pm, anchor: int, target: int, *, radius: int = 0x200
) -> List[int]:
    # 以 anchor 为中心读 ±radius 字节, 4 字节对齐扫描 i32==target 的所有偏移地址.
    base = anchor - radius
    blob = pm.read_bytes(base, radius * 2)
    if blob is None:
        return []
    needle = (target & 0xFFFFFFFF).to_bytes(4, "little")
    hits: List[int] = []
    off = 0
    while True:
        i = blob.find(needle, off)
        if i < 0:
            break
        if (i & 3) == 0:  # 4 字节对齐
            hits.append(base + i)
        off = i + 1
    return hits


def _dump_i32_grid(pm, anchor: int, *, radius: int = 0x80) -> str:
    # 在 anchor 附近以 i32 形式打印 (offset / hex / signed-int) 三列。
    import struct
    base = anchor - radius
    blob = pm.read_bytes(base, radius * 2)
    if blob is None:
        return "(read failed)"
    lines = []
    for i in range(0, len(blob), 4):
        if i + 4 > len(blob):
            break
        v_u = struct.unpack_from("<I", blob, i)[0]
        v_s = struct.unpack_from("<i", blob, i)[0]
        delta = (base + i) - anchor
        marker = "  <-- HP" if delta == 0 else ""
        lines.append(f"   [{delta:+#06x}] 0x{v_u:08X}  i32={v_s:>12d}{marker}")
    return "\n".join(lines)


def run(*, anchors_path: str = DEFAULT_ANCHORS, timeout: float = 120.0) -> int:
    if not is_admin():
        print("[warn] 未以管理员身份运行, OpenProcess 可能失败。", file=sys.stderr)

    if not os.path.isfile(anchors_path):
        print(f"[fail] anchors 文件不存在: {anchors_path}", file=sys.stderr)
        print("    请先运行: python -m tools.mem_probe.auto_locate")
        return 1

    anchors = _load_anchors(anchors_path)
    uid_hits = [int(s, 16) for s in anchors.get("uid_candidates", [])]
    hp_hits = [int(s, 16) for s in anchors.get("hp_candidates", [])]
    mh_hits = [int(s, 16) for s in anchors.get("max_hp_candidates", [])]
    gt = anchors.get("ground_truth", {})
    print(f"[load] anchors loaded from {anchors_path}")
    print(f"[load] ground_truth: uid={gt.get('uid')} hp={gt.get('hp')} max_hp={gt.get('max_hp')}")
    print(f"[load] candidates: uid={len(uid_hits)} hp={len(hp_hits)} maxhp={len(mh_hits)}")

    print("\n[boot] starting PacketBridge for live HP monitoring...")
    src = _TcpSource()
    try:
        ready = src.wait_ready(timeout=60.0)
        baseline_hp = ready["hp"]
        cur_uid = int(ready["uid"] or 0)
        anchor_uid = int(gt.get("uid") or 0)
        # 进程或角色变了 → anchors 里的堆地址全部作废, 必须重扫
        stale_anchors = (cur_uid != anchor_uid) or (not hp_hits) or (not uid_hits)
        if stale_anchors:
            print(
                f"[stale] anchors uid={anchor_uid} != current uid={cur_uid} "
                f"(或候选集为空); 旧的堆地址已作废, 将重扫"
            )

        try:
            pm = StarProcess()
        except StarProcessError as e:
            print(f"[fail] {e}", file=sys.stderr)
            return 2
        try:
            mods = pm.list_modules()
            print(f"[mem] attached pid={pm.pid}")

            # ── Step 0: anchors 失效时, 用多 HP 快照并集做 fresh scan ──
            if stale_anchors:
                from .scanner import scan as _scan
                if cur_uid > 0:
                    t0 = time.time()
                    uid_hits = _scan(pm, cur_uid, "i64", max_hits=200_000)
                    print(f"[fresh] scan uid={cur_uid}: {len(uid_hits)} hits "
                          f"({time.time()-t0:.1f}s)")
                # HP 跳得太厉害, 单值扫不全 → 多快照并集
                hp_hits = _multi_scan_hp(pm, src, snapshot_window=4.0)
                baseline_hp = src.snapshot()["hp"] or baseline_hp
                mh_hits = []

            # ── Step 1: lockstep 验证 (HP 抖动场景的核心) ──
            # 对 hp_hits 在 N 次 TCP 采样的同一瞬间读, 真地址会高分通过。
            print(f"\n[step] lockstep verify {len(hp_hits)} HP candidates...")
            if len(hp_hits) > 50_000:
                # 候选过多, 先做一次 narrow 抽稀 (用当前 HP)
                cur_hp = src.snapshot()["hp"]
                if cur_hp > 0:
                    t0 = time.time()
                    pre = narrow(pm, hp_hits, cur_hp, "i32")
                    print(f"[prune] narrow by current HP={cur_hp}: "
                          f"{len(hp_hits)} -> {len(pre)} ({(time.time()-t0)*1000:.0f}ms)")
                    if pre:
                        hp_hits = pre
            scored = _verify_lockstep(
                pm, src, hp_hits, field="hp", samples=25, interval=0.1, min_match_ratio=0.5
            )
            for a, s in scored[:10]:
                print(f"   hp@{_fmt_addr(pm, a, mods)}  score={s}")
            hp_refined = [a for a, _ in scored]
            print(f"[refine] hp candidates: {len(hp_hits)} -> {len(hp_refined)}")
            if not hp_refined:
                print("[fail] lockstep 没有通过任何候选; 重扫一次新 HP 再试")
                from .scanner import scan as _scan2
                retry_hp = src.snapshot()["hp"]
                if retry_hp > 0:
                    t0 = time.time()
                    hp_hits = _scan2(pm, retry_hp, "i32", max_hits=300_000)
                    print(f"[retry] scan hp={retry_hp}: {len(hp_hits)} hits "
                          f"({time.time()-t0:.1f}s)")
                    scored = _verify_lockstep(
                        pm, src, hp_hits, field="hp", samples=30, interval=0.1, min_match_ratio=0.5
                    )
                    for a, s in scored[:10]:
                        print(f"   hp@{_fmt_addr(pm, a, mods)}  score={s}")
                    hp_refined = [a for a, _ in scored]
                if not hp_refined:
                    print("[fail] 重试仍无候选; 放弃")
                    return 4

            # ── Step 2: 用 *当前* TCP MaxHP narrow mh_hits ──
            cur_snap = src.snapshot()
            cur_max = cur_snap["max_hp"]
            if cur_max <= 0:
                print(f"[warn] 当前 TCP MaxHP={cur_max}, 跳过 maxhp narrow"
                      " (使用全部 max_hp_candidates 作为邻接候选)")
                mh_refined = list(mh_hits)
            else:
                t0 = time.time()
                mh_refined = narrow(pm, mh_hits, cur_max, "i32")
                print(
                    f"[refine] maxhp narrowed by MaxHP={cur_max}: "
                    f"{len(mh_hits)} -> {len(mh_refined)} ({(time.time()-t0)*1000:.0f}ms)"
                )
                # 如果 anchors 里 max_hp_candidates 是基于 0 扫的, 全空很正常 → 重新全扫
                if len(mh_refined) == 0 and cur_max > 0:
                    print(f"[refine] mh_hits 与当前 MaxHP 不匹配 (anchors 可能陈旧),"
                          " 重新全扫...")
                    from .scanner import scan as _scan
                    t0 = time.time()
                    mh_refined = _scan(pm, cur_max, "i32", max_hits=300_000)
                    print(f"[refine] re-scanned MaxHP={cur_max}: {len(mh_refined)} candidates"
                          f" ({(time.time()-t0):.1f}s)")

            # ── Step 4: 邻接对 (hp_addr ± delta == maxhp_addr) ──
            pairs = _find_pairs(hp_refined, mh_refined)
            print(f"\n[pair] hp/maxhp 邻接对 (全局扫): {len(pairs)}")
            for h, m, d in pairs:
                print(f"   hp@{_fmt_addr(pm, h, mods)}  +0x{d:X}  -> maxhp@{_fmt_addr(pm, m, mods)}")

            # ── Step 4.5: HP 已唯一 但全局没配上时, 用 HP 邻域局部搜 MaxHP ──
            # MaxHP 会随技能浮动, 全图扫不准; 但 self_max_hp 必然在 self_hp 同一对象内,
            # 直接读 hp_addr ±0x200 的 1024 字节, 对齐扫 i32 == cur_max_hp 即可。
            if not pairs and len(hp_refined) <= 3 and cur_max > 0:
                print(f"\n[local] 全局没配上, 改用 HP 邻域 ±0x200 找 MaxHP={cur_max}:")
                local_pairs: List[Tuple[int, int, int]] = []
                for h in hp_refined:
                    local_max = _local_find_i32(pm, h, cur_max, radius=0x200)
                    print(f"   hp@{_fmt_addr(pm, h, mods)}: {len(local_max)} local maxhp hits")
                    for lm in local_max:
                        delta = lm - h
                        print(f"      maxhp@{_fmt_addr(pm, lm, mods)} (delta={delta:+#x})")
                        local_pairs.append((h, lm, delta))
                    if not local_max:
                        # 给个 i32 网格, 让人肉眼也能看一眼
                        print(f"   附近 i32 网格 (anchor=hp_addr ±0x80):")
                        print(_dump_i32_grid(pm, h, radius=0x80))
                pairs = local_pairs
                print(f"[local] 总配对: {len(pairs)}")

            # ── Step 5: 邻接对附近找 UID ──
            # 优先用 anchors 里的 uid_hits 做空间过滤; 若没命中,
            # 再在 hp_addr ±0x2000 局部读一遍, 直接精确搜索 ground truth UID。
            print(f"\n[final] 在每个 (hp,maxhp) pair ±0x{_UID_NEAR_RADIUS:X} 内找 UID 候选:")
            chosen: Optional[Tuple[int, int, int, int]] = None  # (hp, maxhp, uid, struct_base)
            gt_uid = int(ready.get("uid") or 0)
            uid_needle = (gt_uid & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little") if gt_uid else b""
            for h, m, d in pairs:
                near = _find_nearby_uid(uid_hits, h, radius=_UID_NEAR_RADIUS)
                tag = " <-- UNIQUE" if len(near) == 1 else ""
                print(f"   pair hp@{h:016X}: {len(near)} uid in pre-scan nearby{tag}")
                for u in near:
                    print(f"      uid@{_fmt_addr(pm, u, mods)} (uid - hp = {u - h:+#x})")
                # 局部精搜: 直接读 ±0x2000 找 uid_needle
                local_uids: List[int] = []
                if uid_needle:
                    radius = 0x2000
                    base_addr = h - radius
                    region = pm.read_bytes(base_addr, radius * 2)
                    if region is not None:
                        off = 0
                        while True:
                            i = region.find(uid_needle, off)
                            if i < 0:
                                break
                            if (i & 7) == 0:  # 8 字节对齐
                                local_uids.append(base_addr + i)
                            off = i + 1
                    print(f"      local-search ±0x{radius:X} for uid bytes: {len(local_uids)} hit(s)")
                    for u in local_uids:
                        print(f"         uid@{_fmt_addr(pm, u, mods)} (uid - hp = {u - h:+#x})")
                # 决策: 优先 near (老候选交集), 否则用 local 唯一
                cand_uid: Optional[int] = None
                if len(near) == 1:
                    cand_uid = near[0]
                elif len(local_uids) == 1:
                    cand_uid = local_uids[0]
                elif len(local_uids) > 1:
                    # 取距离 hp_addr 最近的那个
                    cand_uid = min(local_uids, key=lambda u: abs(u - h))
                    print(f"      pick nearest: uid@{_fmt_addr(pm, cand_uid, mods)}")
                if cand_uid is not None and chosen is None:
                    base_guess = min(h, m, cand_uid) & ~0xF
                    chosen = (h, m, cand_uid, base_guess)

            # ── 结果 ──
            print("\n" + "=" * 72)
            print("REFINE RESULT")
            print("=" * 72)

            # 即使 UID 没找到, 只要有 (hp, maxhp) 唯一对, 就保存 PoC 可用的最小集
            if chosen is None and len(pairs) == 1:
                h, m, d = pairs[0]
                print(f"[partial] 未找到 UID, 但 (hp, maxhp) 唯一锁定:")
                print(f"   self_hp     = {_fmt_addr(pm, h, mods)}")
                print(f"   self_max_hp = {_fmt_addr(pm, m, mods)} (delta={d:+#x})")
                base_guess = min(h, m) & ~0xF
                chosen = (h, m, 0, base_guess)  # uid=0 表示未确定

            if chosen:
                h, m, u, base = chosen
                print(f"[ok] self_hp     = {_fmt_addr(pm, h, mods)}")
                print(f"[ok] self_max_hp = {_fmt_addr(pm, m, mods)}")
                if u:
                    print(f"[ok] self_uid    = {_fmt_addr(pm, u, mods)}")
                else:
                    print(f"[--] self_uid    = (未确定; 后续 reader 不依赖)")
                print(f"[ok] struct_base ~= {_fmt_addr(pm, base, mods)}")
                # dump 范围扩大: 从 base 起 384 字节, 通常能盖住 vtable + 关键字段
                dump_size = 384
                dump_start = min(base, h, m) & ~0xF
                if u:
                    dump_start = min(dump_start, u & ~0xF)
                print(f"\n[ctx] {dump_size} 字节 from 0x{dump_start:X}:")
                print(_hex_context(pm, dump_start, before=0, after=dump_size))

                anchors["self_hp_addr"] = hex(h)
                anchors["self_max_hp_addr"] = hex(m)
                if u:
                    anchors["self_uid_addr"] = hex(u)
                anchors["player_struct_base_guess"] = hex(base)
                anchors["refine_ts"] = time.time()
                anchors["refine_pid"] = pm.pid
                anchors["refine_baseline_hp"] = baseline_hp
                anchors["refine_final_hp"] = src.snapshot()["hp"]
                anchors["refine_final_max_hp"] = src.snapshot()["max_hp"]
                anchors["hp_candidates"] = [hex(a) for a in hp_refined]
                anchors["max_hp_candidates"] = [hex(a) for a in mh_refined]
                _save_anchors(anchors_path, anchors)
                print(f"\n[ok] anchors updated -> {anchors_path}")
                return 0 if u else 0  # partial 也算成功 (PoC 不强求 uid)
            else:
                print("[warn] 未能唯一定位; 当前所有 hp 候选 + 邻近 uid 列表:")
                for h, m, d in pairs:
                    near = _find_nearby_uid(uid_hits, h)
                    print(f"   hp@{_fmt_addr(pm, h, mods)} (delta={d:+#x}, {len(near)} uid nearby)")
                # 仍然把"已收敛的 hp/maxhp"列表写回 (即便 uid 还含混)
                anchors["hp_candidates"] = [hex(a) for a in hp_refined]
                anchors["max_hp_candidates"] = [hex(a) for a in mh_refined]
                anchors["refine_ts"] = time.time()
                _save_anchors(anchors_path, anchors)
                print(
                    "\n[hint] 再跑一次 refine, 让 HP 在不同档位再变一次, 进一步收敛。"
                )
                return 3
        finally:
            pm.close()
    finally:
        src.stop()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.refine")
    p.add_argument("--anchors", default=DEFAULT_ANCHORS, help="anchors.json 路径")
    p.add_argument("--timeout", type=float, default=120.0, help="等待 HP 下降的超时 (秒)")
    args = p.parse_args(argv)
    try:
        return run(anchors_path=args.anchors, timeout=args.timeout)
    except KeyboardInterrupt:
        print("\n[abort] user interrupted")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
