# 混合 TCP + 内存数据源

> 版本 `5.0.0`。配套文档：`ACT_PLATFORM.md`、`PLUGIN_SDK.md`。

## 概述

SAO ACT UI 支持 TCP 抓包和内存读取两种数据源，可单独或混合使用。`mem_probe/unified_source.py` 提供统一的 `UnifiedDataSource` 入口。

## 运行模式

| 模式 | 说明 |
|------|------|
| `tcp` | 纯 TCP 抓包，不启动内存扫描 |
| `memory` | 纯内存读取 |
| `hybrid` | TCP 为主，内存补充 O(1) 查询（Boss血条、自身状态） |
| `auto` | 根据权限和环境自动选择 hybrid 或 tcp |

## 架构

```
插件注册数据源
  ↓
UnifiedDataSource(mode, state_mgr, packet_bridge, ...)
  ↓
set_bridge_classes(MemStateBridge, MemSelfStateProvider)  ← 插件注入
  ↓
_MemStateBridge(state_mgr, dps_tracker, ...)
  ↓
mem_probe.process.GameProcess → 附加目标进程
  ↓
mem_probe.cy_memscan → AVX2 扫描 / 纯 Python fallback
```

## 插件桥接注入

`mem_probe/` 是通用基础设施，游戏特化桥接由插件注入：

```python
from mem_probe.unified_source import set_bridge_classes
from .mem.il2cpp.mem_state_bridge import MemStateBridge
from .mem.il2cpp.mem_self_state_provider import MemSelfStateProvider

set_bridge_classes(MemStateBridge, MemSelfStateProvider)
```

未注入时，`UnifiedDataSource` 无法启动内存桥接，自动回退 TCP-only。

## 策略字段

`UnifiedDataSource._build_policy()` 返回：

| 字段 | 说明 |
|------|------|
| `auto_scan_enabled` | 是否允许自动扫描 |
| `auto_scan_interval_s` | 轮询间隔（默认 0.5s） |
| `require_admin` / `admin_ok` | 管理员权限检测 |
| `max_scan_regions_mb` | 最大扫描区域 |
| `allow_full_heap_scan` | 是否允许全堆扫描 |
| `cy_memscan` | Cython 后端信息 |
| `defer_until_tcp_scene` | 延迟到 TCP 场景事件后再启动 |
| `start_allowed` / `fallback_reason` | 是否可启动 / 回退原因 |

## 健康状态

`health()` 返回：

- `data_source`、`requested_mode`、`mode`、`running`
- `trigger_count`、`last_trigger`
- `alive`（桥接是否存活）、`is_memory_active`
- `policy`（完整策略）
- `self`（uid、hp、max_hp、name、is_dead）
- `watchers`（每个数据维度的来源：memory_first / tcp_fallback）

## TCP 回退

- 内存桥接启动失败时自动回退 TCP-only
- `hybrid` 模式下 DPS 行和战斗事件流始终走 TCP
- 内存仅提供 O(1) 补充（Boss 血条、自身基础属性、场景 ID）
- 回退原因暴露在 `health().fallback_reason`

## GameProcess

`mem_probe.process.GameProcess`：

- 进程名由调用方或游戏插件显式传入，不在平台配置里硬编码
- 需要管理员权限（`PROCESS_VM_READ`）
- 提供 `read_bytes(addr, size)`、`read_uint32/64(addr)`、`modules()`、`memory_regions()`
- 统一读取路径：driver → NtReadVirtualMemory → ReadProcessMemory

## 验证

```bash
python -m act_replay.selftest
# 实测需要管理员 + 游戏进程运行
```
