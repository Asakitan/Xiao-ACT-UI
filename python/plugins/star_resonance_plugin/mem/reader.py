# MemoryReader — 双轨对比 PoC.
#
# 从 anchors.json 读取已定位的 self HP / MaxHP / (可选) UID 地址,
# 后台轮询 ReadProcessMemory, 与 PacketBridge 提供的 TCP ground truth 比对,
# 结果只写入日志, 不接入 GUI / DPS / Boss 任何主链路。
#
# 启动方式:
# 1. 命令行独立: python -m tools.mem_probe.reader  (会自启 PacketBridge 做对比)
# 2. 主程序集成: 当 config.MEM_PROBE_ENABLED=True, main.py 在 bridge 启动后
# 调用 attach_to(state_mgr, parser_provider) 启动后台对比线程
# (本 PoC 阶段不在 main.py 接入, 留 hook)
#
# 输出:
# sao_auto/perf_probe.log 风格, 每条:
# [mem-vs-tcp] ts=... hp mem=12345 tcp=12345 dt=2ms
# [mem-vs-tcp] DRIFT hp mem=12300 tcp=12345 (game updated, mem stale)

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from typing import Any, Callable, Dict, Optional

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from .process import StarProcess, StarProcessError, is_admin

DEFAULT_ANCHORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")
DEFAULT_LOG = os.path.join(_SAO_AUTO_ROOT, "perf_probe.log")


# ───────────────────────── Reader ─────────────────────────
class MemoryReader:
    # 50ms 轮询 self HP/MaxHP 的后台线程, 通过回调暴露读到的值.
    #
    # 读失败/进程消失时自动停, 不抛。

    def __init__(
        self,
        hp_addr: int,
        max_hp_addr: int,
        *,
        uid_addr: Optional[int] = None,
        poll_interval: float = 0.05,
        on_sample: Optional[Callable[[Dict[str, Any]], None]] = None,
    ) -> None:
        self._hp_addr = int(hp_addr)
        self._max_hp_addr = int(max_hp_addr)
        self._uid_addr = int(uid_addr) if uid_addr else 0
        self._interval = float(poll_interval)
        self._on_sample = on_sample
        self._pm: Optional[StarProcess] = None
        self._thr: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._latest: Dict[str, Any] = {}
        self._lock = threading.Lock()
        self._cleanup_lock = threading.Lock()
        self._cleanup_thread: Optional[threading.Thread] = None
        self._read_fail_count = 0

    def start(self) -> None:
        with self._cleanup_lock:
            if self._cleanup_thread is not None:
                return
        if self._thr is not None and self._thr.is_alive():
            return
        if self._thr is not None:
            self._close_process_after(self._thr)
        self._pm = StarProcess()
        self._stop.clear()
        self._thr = threading.Thread(target=self._loop, name="mem_reader", daemon=True)
        self._thr.start()

    def _close_process_after(self, worker: Optional[threading.Thread]) -> None:
        if worker is not None and worker is not threading.current_thread():
            worker.join()
        with self._cleanup_lock:
            if worker is not None and self._thr is worker:
                self._thr = None
            pm, self._pm = self._pm, None
            self._cleanup_thread = None
        if pm is not None:
            try:
                pm.close()
            except Exception:
                pass

    def stop(self, join_timeout: float = 1.0) -> bool:
        self._stop.set()
        worker = self._thr
        if worker is not None and worker is not threading.current_thread():
            worker.join(timeout=max(0.0, float(join_timeout)))
        if worker is not None and worker.is_alive():
            # A heap read can outlive the UI shutdown timeout.  Closing its
            # GameProcess here would detach the process while the worker still
            # owns it.  Defer final cleanup until rundown is proven complete.
            with self._cleanup_lock:
                if self._cleanup_thread is None:
                    finalizer = threading.Thread(
                        target=self._close_process_after,
                        args=(worker,),
                        name="mem-reader-rundown",
                        daemon=True,
                    )
                    self._cleanup_thread = finalizer
                    finalizer.start()
            return False
        self._close_process_after(worker)
        return True

    def latest(self) -> Dict[str, Any]:
        with self._lock:
            return dict(self._latest)

    def _loop(self) -> None:
        assert self._pm is not None
        pm = self._pm
        while not self._stop.is_set():
            t0 = time.time()
            hp = pm.read_i32(self._hp_addr)
            maxhp = pm.read_i32(self._max_hp_addr)
            uid = pm.read_i64(self._uid_addr) if self._uid_addr else None
            dt_ms = (time.time() - t0) * 1000.0
            if hp is None or maxhp is None:
                self._read_fail_count += 1
                if self._read_fail_count >= 20:
                    print(f"[mem-reader] 连续 {self._read_fail_count} 次读失败, 终止线程")
                    break
            else:
                self._read_fail_count = 0
                sample = {
                    "ts": time.time(),
                    "hp": int(hp),
                    "max_hp": int(maxhp),
                    "uid": int(uid) if uid is not None else None,
                    "read_ms": round(dt_ms, 2),
                }
                with self._lock:
                    self._latest = sample
                if self._on_sample:
                    try:
                        self._on_sample(sample)
                    except Exception as e:
                        print(f"[mem-reader] on_sample exception: {e}")
            # 节奏: 不依赖 read 耗时, 简单 sleep
            self._stop.wait(self._interval)


# ───────────────────────── 双轨对比 (CLI 入口) ─────────────────────────
def _load_anchors(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _open_log(path: str):
    f = open(path, "a", encoding="utf-8", buffering=1)  # 行缓冲
    f.write(f"\n# ── mem-vs-tcp session start {time.strftime('%Y-%m-%d %H:%M:%S')} ──\n")
    return f


def run(
    *,
    anchors_path: str = DEFAULT_ANCHORS,
    log_path: str = DEFAULT_LOG,
    duration: float = 0.0,
    poll_interval: float = 0.05,
) -> int:
    if not is_admin():
        print("[warn] 未以管理员身份运行, OpenProcess 可能失败。", file=sys.stderr)
    if not os.path.isfile(anchors_path):
        print(f"[fail] 找不到 {anchors_path}; 请先跑 auto_locate + refine", file=sys.stderr)
        return 1
    anchors = _load_anchors(anchors_path)

    # ── 优先级: fingerprint > pointer_chain > anchors 固定地址 ──
    hp_addr: Optional[int] = None
    max_hp_addr: Optional[int] = None
    uid_addr: Optional[int] = None
    pm_for_resolve: Optional[StarProcess] = None

    fp = anchors.get("fingerprint")
    fp_v2 = anchors.get("fingerprint_v2")
    chain = anchors.get("pointer_chain")
    if fp or fp_v2 or chain:
        try:
            pm_for_resolve = StarProcess()
        except StarProcessError as e:
            print(f"[warn] resolve 阶段 attach 失败 ({e}); 回退到固定地址")

    # v2 优先 (模块相对指针, 跨进程最稳)
    if pm_for_resolve is not None and fp_v2:
        try:
            from .fingerprint_v2 import locate_v2 as fpv2_locate
            matches = fpv2_locate(pm_for_resolve, fp_v2)
            valid = [m for m in matches if 1 <= (pm_for_resolve.read_i32(m) or 0) <= 10_000_000]
            if len(valid) == 1:
                hp_addr = valid[0]
                max_hp_addr = hp_addr + fp_v2["delta_max_hp"]
                print(f"[fingerprint_v2] resolved self_hp = 0x{hp_addr:X}")
            elif len(matches) == 1:
                hp_addr = matches[0]
                max_hp_addr = hp_addr + fp_v2["delta_max_hp"]
                print(f"[fingerprint_v2] resolved self_hp = 0x{hp_addr:X} (HP 值未校验)")
            else:
                print(f"[fingerprint_v2] {len(matches)} 处匹配 ({len(valid)} HP 合理); 回退")
        except Exception as e:
            print(f"[warn] fingerprint_v2 异常 ({e}); 回退")

    if hp_addr is None and pm_for_resolve is not None and fp:
        try:
            from .fingerprint import locate as fp_locate
            matches = fp_locate(pm_for_resolve, fp)
            if len(matches) == 1:
                hp_addr = matches[0]
                max_hp_addr = hp_addr + fp["delta_max_hp"]
                if anchors.get("self_uid_addr"):
                    delta_uid = int(anchors["self_uid_addr"], 16) - int(
                        anchors["self_hp_addr"], 16
                    )
                    uid_addr = hp_addr + delta_uid
                print(f"[fingerprint] resolved self_hp = 0x{hp_addr:X} (唯一匹配)")
            elif len(matches) > 1:
                print(f"[fingerprint] {len(matches)} 处匹配, 不能唯一确定; 回退")
            else:
                print("[fingerprint] 0 匹配; 回退")
        except Exception as e:
            print(f"[warn] fingerprint locate 异常 ({e}); 回退")

    if hp_addr is None and pm_for_resolve is not None and chain:
        try:
            from .pointer_chain import resolve_chain
            resolved = resolve_chain(pm_for_resolve, chain)
            if resolved is not None:
                hp_addr = resolved
                final_off = int(chain["final_offset"], 16)
                obj_base = resolved - final_off
                delta_max = int(anchors.get("self_max_hp_addr", "0x0"), 16) - int(
                    anchors.get("self_hp_addr", "0x0"), 16
                )
                max_hp_addr = resolved + delta_max
                if anchors.get("self_uid_addr"):
                    delta_uid = int(anchors["self_uid_addr"], 16) - int(
                        anchors["self_hp_addr"], 16
                    )
                    uid_addr = resolved + delta_uid
                print(f"[chain] resolved self_hp = 0x{hp_addr:X}")
            else:
                print("[chain] resolve 失败; 回退")
        except Exception as e:
            print(f"[warn] chain resolve 异常 ({e}); 回退")

    if pm_for_resolve is not None:
        pm_for_resolve.close()

    # 回退: 用 anchors 里的固定堆地址 (跨重启会失效, 仅当链不存在时)
    if hp_addr is None:
        hp_addr_s = anchors.get("self_hp_addr")
        max_hp_addr_s = anchors.get("self_max_hp_addr")
        uid_addr_s = anchors.get("self_uid_addr")
        if hp_addr_s and max_hp_addr_s:
            hp_addr = int(hp_addr_s, 16)
            max_hp_addr = int(max_hp_addr_s, 16)
            uid_addr = int(uid_addr_s, 16) if uid_addr_s else None
            # 跨重启时固定地址几乎肯定失效, 用 sanity check 快速验证
            try:
                pm_check = StarProcess()
                from .auto_anchor import is_addr_sane
                if not is_addr_sane(pm_check, hp_addr, max_hp_addr):
                    print(f"[anchors] 固定地址不合理 (HP/Max 越界), 触发自动重定位")
                    hp_addr = None
                pm_check.close()
            except Exception as e:
                print(f"[warn] sanity check 异常 ({e}); 仍尝试用固定地址")

    # 终极回退: 启动 TCP src 后跑一次完整 auto-relocate
    auto_relocated = False
    if hp_addr is None:
        print("[anchors] 所有锚定方式失效, 启动自动重定位 (需要游戏在线 + packet 抓到)")
        from .auto_locate import _TcpSource
        from .auto_anchor import relocate_now
        relocate_src = _TcpSource()
        try:
            relocate_src.wait_ready(timeout=120.0)
            pm_relocate = StarProcess()
            try:
                result = relocate_now(pm_relocate, relocate_src, hp_timeout=90.0)
            finally:
                pm_relocate.close()
            if result and result.get("hp_addr"):
                hp_addr = result["hp_addr"]
                max_hp_addr = result["max_hp_addr"]
                uid_addr = result["uid_addr"]
                auto_relocated = True
                print(f"[relocate] 成功; 复用 TCP src 进入主循环")
                # 复用 src
                src = relocate_src
            else:
                print(f"[fail] 自动重定位失败")
                relocate_src.stop()
                return 1
        except Exception as e:
            print(f"[fail] 自动重定位异常: {e}", file=sys.stderr)
            try:
                relocate_src.stop()
            except Exception:
                pass
            return 1

    if hp_addr is None or max_hp_addr is None:
        print(f"[fail] hp_addr / max_hp_addr 未定位", file=sys.stderr)
        return 1
    print(f"[anchors] hp=0x{hp_addr:X} max_hp=0x{max_hp_addr:X} "
          f"uid={'0x%X' % uid_addr if uid_addr else '(none)'}")

    # 启动 TCP 源做对比 (auto_relocate 路径已经启动了 src, 复用)
    if not auto_relocated:
        print("[boot] starting PacketBridge for ground truth...")
        from .auto_locate import _TcpSource
        src = _TcpSource()
    log_f = _open_log(log_path)
    log_f.write(f"# anchors hp=0x{hp_addr:X} max_hp=0x{max_hp_addr:X} "
                f"uid={'0x%X' % uid_addr if uid_addr else 'none'}\n")

    # 统计
    stats = {
        "samples": 0,
        "match_hp": 0,
        "drift_hp": 0,
        "max_drift": 0,
        "mem_lead": 0,
        "tcp_lead": 0,
    }

    def on_sample(sample: Dict[str, Any]) -> None:
        stats["samples"] += 1
        tcp_snap = src.snapshot()
        tcp_hp = tcp_snap["hp"]
        mem_hp = sample["hp"]
        if tcp_hp <= 0:
            return  # 还没 TCP 数据
        if mem_hp == tcp_hp:
            stats["match_hp"] += 1
            # 默认不打 match, 减少 log 噪声; 每 100 个采样打一次心跳
            if stats["samples"] % 100 == 0:
                log_f.write(
                    f"[mem-vs-tcp] heartbeat samples={stats['samples']} "
                    f"match={stats['match_hp']} drift={stats['drift_hp']} "
                    f"hp={mem_hp} max={sample['max_hp']} read_ms={sample['read_ms']}\n"
                )
        else:
            stats["drift_hp"] += 1
            diff = mem_hp - tcp_hp
            if abs(diff) > stats["max_drift"]:
                stats["max_drift"] = abs(diff)
            if diff > 0:
                stats["mem_lead"] += 1
            else:
                stats["tcp_lead"] += 1
            log_f.write(
                f"[mem-vs-tcp] DRIFT mem_hp={mem_hp} tcp_hp={tcp_hp} "
                f"diff={diff:+d} max_hp_mem={sample['max_hp']} "
                f"max_hp_tcp={tcp_snap['max_hp']} ts={sample['ts']:.3f}\n"
            )

    try:
        if not auto_relocated:
            src.wait_ready(timeout=60.0)
        print("[boot] TCP ready; starting MemoryReader...")
        reader = MemoryReader(
            hp_addr, max_hp_addr, uid_addr=uid_addr,
            poll_interval=poll_interval, on_sample=on_sample,
        )
        try:
            reader.start()
        except StarProcessError as e:
            print(f"[fail] {e}", file=sys.stderr)
            return 2

        print(f"[run] polling every {poll_interval*1000:.0f}ms; "
              f"duration={'unlimited' if duration <= 0 else f'{duration}s'}; "
              f"Ctrl+C to stop")
        deadline = time.time() + duration if duration > 0 else None
        last_print = 0.0
        last_health_check = time.time()
        last_relocate_attempt = 0.0
        from .auto_anchor import is_addr_sane, relocate_now
        try:
            while True:
                time.sleep(0.5)
                if deadline and time.time() >= deadline:
                    break
                now = time.time()

                # ── Watchdog: 每 30s 检查一次 anchor 是否还合理 ──
                # 触发条件: HP 越界 OR 连续 drift > 80% 且采样 > 50
                if now - last_health_check >= 30.0:
                    last_health_check = now
                    sane = True
                    try:
                        pm_check = StarProcess()
                        sane = is_addr_sane(pm_check, hp_addr, max_hp_addr)
                        pm_check.close()
                    except Exception:
                        sane = False

                    s = stats["samples"]
                    drift_pct = (stats["drift_hp"] / s * 100.0) if s > 0 else 0
                    needs_relocate = (not sane) or (s > 50 and drift_pct > 80.0)

                    if needs_relocate and (now - last_relocate_attempt) > 60.0:
                        print(f"[watchdog] anchor 失效 (sane={sane}, drift={drift_pct:.1f}%), "
                              f"触发自动重定位")
                        last_relocate_attempt = now
                        try:
                            reader.stop()
                            pm_relocate = StarProcess()
                            try:
                                result = relocate_now(pm_relocate, src, hp_timeout=60.0)
                            finally:
                                pm_relocate.close()
                            if result and result.get("hp_addr"):
                                hp_addr = result["hp_addr"]
                                max_hp_addr = result["max_hp_addr"]
                                uid_addr = result["uid_addr"]
                                # 重置统计
                                for k in stats:
                                    stats[k] = 0
                                reader = MemoryReader(
                                    hp_addr, max_hp_addr, uid_addr=uid_addr,
                                    poll_interval=poll_interval, on_sample=on_sample,
                                )
                                reader.start()
                                print(f"[watchdog] 重定位成功; 新 anchor hp=0x{hp_addr:X}")
                            else:
                                print(f"[watchdog] 重定位失败, 继续用旧 anchor")
                                reader.start()
                        except Exception as e:
                            print(f"[watchdog] 重定位异常: {e}; 继续用旧 anchor")
                            try:
                                reader.start()
                            except Exception:
                                pass

                if now - last_print >= 5.0:
                    last_print = now
                    s = stats["samples"]
                    if s == 0:
                        print(f"[stats] samples=0 (waiting first sample)")
                    else:
                        rate = stats["match_hp"] / s * 100.0
                        print(
                            f"[stats] samples={s} match={stats['match_hp']} ({rate:.1f}%) "
                            f"drift={stats['drift_hp']} max|d|={stats['max_drift']} "
                            f"mem_lead={stats['mem_lead']} tcp_lead={stats['tcp_lead']}"
                        )
        except KeyboardInterrupt:
            print("\n[abort] user interrupt")
        finally:
            reader.stop()
        # 最终
        print("\n=== FINAL ===")
        print(f"samples = {stats['samples']}")
        if stats["samples"] > 0:
            rate = stats["match_hp"] / stats["samples"] * 100.0
            print(f"match   = {stats['match_hp']} ({rate:.2f}%)")
        print(f"drift   = {stats['drift_hp']} (max |diff| = {stats['max_drift']})")
        print(f"        mem_lead={stats['mem_lead']} tcp_lead={stats['tcp_lead']}")
        log_f.write(f"# session end samples={stats['samples']} match={stats['match_hp']} "
                    f"drift={stats['drift_hp']} max_drift={stats['max_drift']}\n")
        return 0
    finally:
        src.stop()
        log_f.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.reader")
    p.add_argument("--anchors", default=DEFAULT_ANCHORS, help="anchors.json 路径")
    p.add_argument("--log", default=DEFAULT_LOG, help="对比日志路径")
    p.add_argument("--duration", type=float, default=0.0, help="运行时长 (秒, 0=无限)")
    p.add_argument("--interval", type=float, default=0.05, help="轮询间隔 (秒, 默认 50ms)")
    args = p.parse_args(argv)
    return run(
        anchors_path=args.anchors,
        log_path=args.log,
        duration=args.duration,
        poll_interval=args.interval,
    )


if __name__ == "__main__":
    raise SystemExit(main())
