"""SAO engine interfaces exposed as LLM-callable tools.

Each tool wraps a real engine API so the LLM can inspect and control the ACT
runtime.  Tools are grouped by category and registered in the global
ToolRegistry on ``register_engine_tools(gui_ref)``.
"""

from __future__ import annotations

import json
import time
from typing import Any, Dict, List, Optional

from ai_editor.tool_registry import ToolRegistry


def register_engine_tools(registry: ToolRegistry, gui_ref: Any) -> None:
    """Register all engine tools, using *gui_ref* to reach live state."""

    # ── helpers ───────────────────────────────────────────────────────────
    def _settings():
        return getattr(gui_ref, 'settings', None) or getattr(gui_ref, '_cfg_settings_ref', None)

    def _state():
        return getattr(gui_ref, '_game_state', None) or {}

    def _dps():
        return getattr(gui_ref, '_dps_tracker', None)

    def _packet_bridge():
        return getattr(gui_ref, '_packet_bridge', None)

    def _mem_bridge():
        pb = _packet_bridge()
        if pb:
            return getattr(pb, '_unified_data_source', None)
        return getattr(gui_ref, '_mem_bridge', None)

    def _encounter():
        return getattr(gui_ref, '_encounter_manager', None)

    def _boss_raid():
        return getattr(gui_ref, '_boss_raid_engine', None)

    def _trigger_engine():
        return getattr(gui_ref, '_trigger_engine', None)

    def _auto_key():
        return getattr(gui_ref, '_auto_key_engine', None)

    def _plugin_manager():
        return getattr(gui_ref, '_plugin_manager', None)

    # ==================================================================
    # Category: game_state — read-only game state queries
    # ==================================================================

    registry.register(
        name="get_game_state",
        description="获取当前游戏状态快照: 角色名/UID/等级/职业/HP/场景等",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _format_game_state(gui_ref),
        category="game_state",
    )

    registry.register(
        name="get_self_entity",
        description="获取自身实体完整属性 (HP/MP/攻击力/防御等全部attr)",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _get_self_entity(gui_ref),
        category="game_state",
    )

    registry.register(
        name="get_entity_list",
        description="获取当前视野内所有实体列表 (玩家/怪物/NPC)",
        parameters={
            "type": "object",
            "properties": {
                "kind": {
                    "type": "string",
                    "description": "过滤类型: player/monster/npc/all",
                    "default": "all",
                },
                "limit": {
                    "type": "integer",
                    "description": "最多返回数量",
                    "default": 50,
                },
            },
        },
        handler=lambda kind="all", limit=50: _get_entity_list(gui_ref, kind, int(limit)),
        category="game_state",
    )

    registry.register(
        name="get_entity_detail",
        description="获取指定实体的详细属性 (按UUID或名字)",
        parameters={
            "type": "object",
            "properties": {
                "uuid": {"type": "integer", "description": "实体UUID"},
                "name": {"type": "string", "description": "实体名字 (模糊匹配)"},
            },
        },
        handler=lambda uuid=0, name="": _get_entity_detail(gui_ref, int(uuid) if uuid else 0, str(name)),
        category="game_state",
    )

    # ==================================================================
    # Category: dps — DPS tracker and combat data
    # ==================================================================

    registry.register(
        name="get_dps_summary",
        description="获取当前/上次战斗的DPS汇总: 每人总伤/DPS/占比",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _get_dps_summary(gui_ref),
        category="dps",
    )

    registry.register(
        name="get_combat_status",
        description="获取战斗状态: 是否战斗中/持续时间/参与人数/Boss信息",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _get_combat_status(gui_ref),
        category="dps",
    )

    registry.register(
        name="get_skill_breakdown",
        description="获取指定玩家的技能伤害分解",
        parameters={
            "type": "object",
            "properties": {
                "player_name": {"type": "string", "description": "玩家名字"},
            },
            "required": ["player_name"],
        },
        handler=lambda player_name: _get_skill_breakdown(gui_ref, player_name),
        category="dps",
    )

    # ==================================================================
    # Category: boss — Boss状态和Raid
    # ==================================================================

    registry.register(
        name="get_boss_status",
        description="获取当前Boss状态: HP/破韧值/护盾/技能/机制",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _get_boss_status(gui_ref),
        category="boss",
    )

    registry.register(
        name="get_raid_mechanics",
        description="获取Boss机制列表 (已配置的Raid机制)",
        parameters={
            "type": "object",
            "properties": {
                "boss_id": {"type": "integer", "description": "Boss模板ID (0=当前Boss)"},
            },
        },
        handler=lambda boss_id=0: _get_raid_mechanics(gui_ref, int(boss_id)),
        category="boss",
    )

    # ==================================================================
    # Category: memory — 内存读取 (低级接口)
    # ==================================================================

    registry.register(
        name="mem_read_bytes",
        description="读取进程内存指定地址的原始字节 (hex)",
        parameters={
            "type": "object",
            "properties": {
                "address": {"type": "string", "description": "内存地址 (十六进制, 如 '0x7FF12345')"},
                "size": {"type": "integer", "description": "读取字节数", "default": 64},
            },
            "required": ["address"],
        },
        handler=lambda address, size=64: _mem_read_bytes(gui_ref, address, int(size)),
        category="memory",
        requires_confirm=True,
    )

    registry.register(
        name="mem_read_string",
        description="读取内存中的字符串 (UTF-16LE, Il2Cpp String)",
        parameters={
            "type": "object",
            "properties": {
                "address": {"type": "string", "description": "字符串对象地址 (十六进制)"},
            },
            "required": ["address"],
        },
        handler=lambda address: _mem_read_string(gui_ref, address),
        category="memory",
    )

    registry.register(
        name="mem_scan_pattern",
        description="在内存中搜索字节模式 (AOB scan)",
        parameters={
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "字节模式, 如 '48 8B 05 ?? ?? ?? ?? 48 85 C0'"},
                "module": {"type": "string", "description": "模块名 (如 'GameAssembly.dll'), 空=全进程"},
                "limit": {"type": "integer", "description": "最多返回结果数", "default": 10},
            },
            "required": ["pattern"],
        },
        handler=lambda pattern, module="", limit=10: _mem_scan_pattern(gui_ref, pattern, module, int(limit)),
        category="memory",
        requires_confirm=True,
    )

    registry.register(
        name="mem_get_module_info",
        description="获取进程模块信息 (基址/大小/路径)",
        parameters={
            "type": "object",
            "properties": {
                "module_name": {"type": "string", "description": "模块名, 空=列出所有模块", "default": ""},
            },
        },
        handler=lambda module_name="": _mem_get_module_info(gui_ref, module_name),
        category="memory",
    )

    # ==================================================================
    # Category: trigger — 触发器/计时器
    # ==================================================================

    registry.register(
        name="get_triggers",
        description="获取所有触发器/计时器配置和状态",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _get_triggers(gui_ref),
        category="trigger",
    )

    registry.register(
        name="fire_trigger",
        description="手动触发一个触发器",
        parameters={
            "type": "object",
            "properties": {
                "trigger_id": {"type": "string", "description": "触发器ID"},
            },
            "required": ["trigger_id"],
        },
        handler=lambda trigger_id: _fire_trigger(gui_ref, trigger_id),
        category="trigger",
        requires_confirm=True,
    )

    # ==================================================================
    # Category: settings — 配置读写
    # ==================================================================

    registry.register(
        name="get_setting",
        description="读取ACT设置项",
        parameters={
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "设置键名"},
            },
            "required": ["key"],
        },
        handler=lambda key: _get_setting(gui_ref, key),
        category="settings",
    )

    registry.register(
        name="set_setting",
        description="写入ACT设置项",
        parameters={
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "设置键名"},
                "value": {"description": "设置值"},
            },
            "required": ["key", "value"],
        },
        handler=lambda key, value: _set_setting(gui_ref, key, value),
        category="settings",
        requires_confirm=True,
    )

    # ==================================================================
    # Category: plugin — 插件管理
    # ==================================================================

    registry.register(
        name="list_plugins",
        description="列出已安装的插件及其状态",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _list_plugins(gui_ref),
        category="plugin",
    )

    registry.register(
        name="toggle_plugin",
        description="启用/禁用指定插件",
        parameters={
            "type": "object",
            "properties": {
                "plugin_id": {"type": "string", "description": "插件ID"},
                "enabled": {"type": "boolean", "description": "true=启用, false=禁用"},
            },
            "required": ["plugin_id", "enabled"],
        },
        handler=lambda plugin_id, enabled: _toggle_plugin(gui_ref, plugin_id, bool(enabled)),
        category="plugin",
        requires_confirm=True,
    )

    # ==================================================================
    # Category: system — 系统/杂项
    # ==================================================================

    registry.register(
        name="get_system_info",
        description="获取ACT系统信息: 版本/运行时间/UI模式/数据源",
        parameters={"type": "object", "properties": {}},
        handler=lambda: _get_system_info(gui_ref),
        category="system",
    )

    registry.register(
        name="eval_python",
        description="在ACT Python环境中执行表达式并返回结果",
        parameters={
            "type": "object",
            "properties": {
                "expression": {"type": "string", "description": "Python表达式"},
            },
            "required": ["expression"],
        },
        handler=lambda expression: _eval_python(gui_ref, expression),
        category="system",
        requires_confirm=True,
    )

    registry.register(
        name="exec_python",
        description="在ACT Python环境中执行代码块 (多行, 有副作用)",
        parameters={
            "type": "object",
            "properties": {
                "code": {"type": "string", "description": "Python代码块"},
            },
            "required": ["code"],
        },
        handler=lambda code: _exec_python(gui_ref, code),
        category="system",
        requires_confirm=True,
    )

    # ==================================================================
    # Category: editor — 编辑器操作 (通过JS bridge控制前端编辑器)
    # ==================================================================

    registry.register(
        name="editor_get_content",
        description="获取编辑器当前内容",
        parameters={"type": "object", "properties": {}},
        handler=lambda: {"note": "Use window.pywebview.api.editor_get_content() from JS"},
        category="editor",
    )

    registry.register(
        name="editor_set_content",
        description="设置编辑器内容 (替换全部)",
        parameters={
            "type": "object",
            "properties": {
                "content": {"type": "string", "description": "新内容"},
                "language": {"type": "string", "description": "语言模式 (python/javascript/json等)", "default": ""},
                "filename": {"type": "string", "description": "文件名 (用于语言检测)", "default": ""},
            },
            "required": ["content"],
        },
        handler=lambda content, language="", filename="": {"note": "Dispatched to editor via JS bridge", "content_length": len(content)},
        category="editor",
    )

    registry.register(
        name="editor_insert_text",
        description="在编辑器光标位置插入文本",
        parameters={
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "要插入的文本"},
            },
            "required": ["text"],
        },
        handler=lambda text: {"note": "Dispatched to editor via JS bridge", "text_length": len(text)},
        category="editor",
    )

    registry.register(
        name="editor_get_selection",
        description="获取编辑器当前选中文本",
        parameters={"type": "object", "properties": {}},
        handler=lambda: {"note": "Use window.pywebview.api.editor_get_selection() from JS"},
        category="editor",
    )

    registry.register(
        name="editor_go_to_line",
        description="跳转到编辑器指定行号",
        parameters={
            "type": "object",
            "properties": {
                "line": {"type": "integer", "description": "行号 (从1开始)"},
            },
            "required": ["line"],
        },
        handler=lambda line: {"note": "Dispatched to editor via JS bridge", "line": line},
        category="editor",
    )

    registry.register(
        name="editor_find_replace",
        description="在编辑器中查找替换",
        parameters={
            "type": "object",
            "properties": {
                "find": {"type": "string", "description": "查找文本"},
                "replace": {"type": "string", "description": "替换文本"},
                "all": {"type": "boolean", "description": "是否全部替换", "default": False},
            },
            "required": ["find", "replace"],
        },
        handler=lambda find, replace, all=False: {"note": "Dispatched to editor via JS bridge"},
        category="editor",
    )

    registry.register(
        name="editor_get_language",
        description="获取编辑器当前语言模式",
        parameters={"type": "object", "properties": {}},
        handler=lambda: {"note": "Use window.pywebview.api.editor_get_language() from JS"},
        category="editor",
    )


# ======================================================================
# Handler implementations
# ======================================================================

def _format_game_state(gui_ref: Any) -> Dict[str, Any]:
    gs = getattr(gui_ref, '_game_state', None) or {}
    return {
        "uid": gs.get("uid", 0),
        "name": gs.get("name", ""),
        "level": gs.get("level", 0),
        "profession": gs.get("profession", ""),
        "hp": gs.get("hp", 0),
        "max_hp": gs.get("max_hp", 0),
        "scene": gs.get("scene", ""),
        "scene_id": gs.get("scene_id", 0),
        "in_combat": gs.get("in_combat", False),
        "server": gs.get("server", ""),
    }


def _get_self_entity(gui_ref: Any) -> Dict[str, Any]:
    gs = getattr(gui_ref, '_game_state', None) or {}
    uid = gs.get("uid", 0)
    rows = getattr(gui_ref, '_rows', None) or {}
    self_row = None
    if isinstance(rows, dict):
        for r in rows.values():
            if isinstance(r, dict) and r.get("uid") == uid:
                self_row = r
                break
    if self_row:
        return {k: v for k, v in self_row.items() if not k.startswith("_")}
    return {"uid": uid, "name": gs.get("name", ""), "note": "entity data not available via DPS rows"}


def _get_entity_list(gui_ref: Any, kind: str, limit: int) -> List[Dict[str, Any]]:
    entities = []
    rows = getattr(gui_ref, '_rows', None) or {}
    if isinstance(rows, dict):
        for r in rows.values():
            if not isinstance(r, dict):
                continue
            rk = r.get("kind", "")
            if kind != "all" and rk != kind:
                continue
            entities.append({
                "uuid": r.get("uuid", 0),
                "name": r.get("name", ""),
                "kind": rk,
                "hp": r.get("hp", 0),
                "max_hp": r.get("max_hp", 0),
                "level": r.get("level", 0),
                "total_damage": r.get("total_damage", 0),
                "dps": r.get("dps", 0),
            })
    return entities[:limit]


def _get_entity_detail(gui_ref: Any, uuid: int, name: str) -> Dict[str, Any]:
    rows = getattr(gui_ref, '_rows', None) or {}
    if isinstance(rows, dict):
        for r in rows.values():
            if not isinstance(r, dict):
                continue
            if uuid and r.get("uuid") == uuid:
                return {k: v for k, v in r.items() if not k.startswith("_")}
            if name and name.lower() in str(r.get("name", "")).lower():
                return {k: v for k, v in r.items() if not k.startswith("_")}
    return {"error": "Entity not found"}


def _get_dps_summary(gui_ref: Any) -> Dict[str, Any]:
    tracker = getattr(gui_ref, '_dps_tracker', None)
    if not tracker:
        return {"error": "DPS tracker not available"}
    try:
        summary = getattr(tracker, 'get_summary', lambda: None)()
        if summary:
            return summary if isinstance(summary, dict) else {"data": str(summary)}
    except Exception:
        pass
    rows = getattr(gui_ref, '_rows', None) or {}
    result = []
    if isinstance(rows, dict):
        for r in rows.values():
            if isinstance(r, dict) and r.get("total_damage", 0) > 0:
                result.append({
                    "name": r.get("name", "?"),
                    "total_damage": r.get("total_damage", 0),
                    "dps": r.get("dps", 0),
                    "pct": r.get("damage_pct", 0),
                })
    return {"players": sorted(result, key=lambda x: x["total_damage"], reverse=True)}


def _get_combat_status(gui_ref: Any) -> Dict[str, Any]:
    enc = getattr(gui_ref, '_encounter_manager', None)
    gs = getattr(gui_ref, '_game_state', None) or {}
    result: Dict[str, Any] = {
        "in_combat": gs.get("in_combat", False),
    }
    if enc:
        result["duration"] = getattr(enc, 'combat_duration', 0)
        result["encounter_count"] = getattr(enc, 'encounter_count', 0)
    return result


def _get_skill_breakdown(gui_ref: Any, player_name: str) -> Dict[str, Any]:
    tracker = getattr(gui_ref, '_dps_tracker', None)
    if not tracker:
        return {"error": "DPS tracker not available"}
    try:
        breakdown = getattr(tracker, 'get_skill_breakdown', None)
        if callable(breakdown):
            return breakdown(player_name)
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "Skill breakdown not implemented"}


def _get_boss_status(gui_ref: Any) -> Dict[str, Any]:
    gs = getattr(gui_ref, '_game_state', None) or {}
    result: Dict[str, Any] = {}
    boss_hp = gs.get("boss_hp") or gs.get("boss_current_hp")
    boss_max = gs.get("boss_max_hp")
    if boss_hp is not None:
        result["boss_hp"] = boss_hp
        result["boss_max_hp"] = boss_max
    result["boss_break"] = gs.get("boss_break", {})
    result["boss_shield"] = gs.get("boss_shield", {})
    result["boss_name"] = gs.get("boss_name", "")
    return result if any(v for v in result.values()) else {"note": "No boss in combat"}


def _get_raid_mechanics(gui_ref: Any, boss_id: int) -> Dict[str, Any]:
    engine = getattr(gui_ref, '_boss_raid_engine', None)
    if not engine:
        return {"error": "Boss raid engine not available"}
    try:
        mechs = getattr(engine, 'get_mechanics', None)
        if callable(mechs):
            return mechs(boss_id) if boss_id else mechs()
    except Exception as exc:
        return {"error": str(exc)}
    return {"note": "No mechanics configured"}


def _mem_read_bytes(gui_ref: Any, address: str, size: int) -> Dict[str, Any]:
    size = min(size, 4096)
    try:
        addr = int(address, 16) if isinstance(address, str) else int(address)
    except ValueError:
        return {"error": f"Invalid address: {address}"}
    bridge = getattr(gui_ref, '_mem_bridge', None)
    if not bridge:
        pb = getattr(gui_ref, '_packet_bridge', None)
        if pb:
            bridge = getattr(pb, '_unified_data_source', None)
    if not bridge:
        return {"error": "Memory bridge not available"}
    reader = getattr(bridge, '_mem_reader', None) or getattr(bridge, 'mem_reader', None)
    if not reader:
        return {"error": "Memory reader not available"}
    try:
        read_fn = getattr(reader, 'read_bytes', None)
        if callable(read_fn):
            data = read_fn(addr, size)
            if data:
                return {
                    "address": hex(addr),
                    "size": len(data),
                    "hex": data.hex(),
                    "ascii": "".join(chr(b) if 32 <= b < 127 else "." for b in data),
                }
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "Read failed"}


def _mem_read_string(gui_ref: Any, address: str) -> Dict[str, Any]:
    try:
        addr = int(address, 16) if isinstance(address, str) else int(address)
    except ValueError:
        return {"error": f"Invalid address: {address}"}
    bridge = getattr(gui_ref, '_mem_bridge', None)
    if not bridge:
        pb = getattr(gui_ref, '_packet_bridge', None)
        if pb:
            bridge = getattr(pb, '_unified_data_source', None)
    if not bridge:
        return {"error": "Memory bridge not available"}
    reader = getattr(bridge, '_mem_reader', None) or getattr(bridge, 'mem_reader', None)
    if not reader:
        return {"error": "Memory reader not available"}
    try:
        read_fn = getattr(reader, 'read_bytes', None)
        if callable(read_fn):
            header = read_fn(addr, 0x20)
            if header and len(header) >= 0x14:
                import struct
                length = struct.unpack_from('<i', header, 0x10)[0]
                if 0 < length < 1024:
                    str_data = read_fn(addr + 0x14, length * 2)
                    if str_data:
                        return {"address": hex(addr), "value": str_data.decode('utf-16-le', errors='replace')}
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "String read failed"}


def _mem_scan_pattern(gui_ref: Any, pattern: str, module: str, limit: int) -> Dict[str, Any]:
    return {"error": "AOB scan via AI editor not yet implemented — use mem_scope panel"}


def _mem_get_module_info(gui_ref: Any, module_name: str) -> Any:
    bridge = getattr(gui_ref, '_mem_bridge', None)
    if not bridge:
        pb = getattr(gui_ref, '_packet_bridge', None)
        if pb:
            bridge = getattr(pb, '_unified_data_source', None)
    if not bridge:
        return {"error": "Memory bridge not available"}
    reader = getattr(bridge, '_mem_reader', None) or getattr(bridge, 'mem_reader', None)
    if not reader:
        return {"error": "Memory reader not available"}
    try:
        modules_fn = getattr(reader, 'enum_modules', None) or getattr(reader, 'list_modules', None)
        if callable(modules_fn):
            mods = modules_fn()
            if module_name:
                mods = [m for m in mods if module_name.lower() in str(m.get('name', '')).lower()]
            return {"modules": mods[:50]}
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "Module enumeration not available"}


def _get_triggers(gui_ref: Any) -> Dict[str, Any]:
    engine = getattr(gui_ref, '_trigger_engine', None)
    if not engine:
        return {"error": "Trigger engine not available"}
    try:
        triggers = getattr(engine, 'get_all_triggers', None)
        if callable(triggers):
            return {"triggers": triggers()}
    except Exception as exc:
        return {"error": str(exc)}
    return {"note": "No triggers configured"}


def _fire_trigger(gui_ref: Any, trigger_id: str) -> Dict[str, Any]:
    engine = getattr(gui_ref, '_trigger_engine', None)
    if not engine:
        return {"error": "Trigger engine not available"}
    try:
        fire = getattr(engine, 'fire_trigger', None)
        if callable(fire):
            fire(trigger_id)
            return {"ok": True, "trigger_id": trigger_id}
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "Fire not available"}


def _get_setting(gui_ref: Any, key: str) -> Any:
    s = getattr(gui_ref, 'settings', None)
    if not s:
        return {"error": "Settings not available"}
    return {"key": key, "value": s.get(key)}


def _set_setting(gui_ref: Any, key: str, value: Any) -> Dict[str, Any]:
    s = getattr(gui_ref, 'settings', None)
    if not s:
        return {"error": "Settings not available"}
    s.set(key, value)
    try:
        s.save()
    except Exception:
        pass
    return {"ok": True, "key": key}


def _list_plugins(gui_ref: Any) -> Dict[str, Any]:
    pm = getattr(gui_ref, '_plugin_manager', None)
    if not pm:
        return {"error": "Plugin manager not available"}
    try:
        plugins = getattr(pm, 'list_plugins', None)
        if callable(plugins):
            return {"plugins": plugins()}
        manifests = getattr(pm, '_manifests', None) or {}
        enabled = getattr(pm, '_enabled', None) or {}
        result = []
        for pid, m in manifests.items():
            result.append({
                "id": pid,
                "name": m.get("name", pid),
                "version": m.get("version", "?"),
                "enabled": enabled.get(pid, False),
            })
        return {"plugins": result}
    except Exception as exc:
        return {"error": str(exc)}


def _toggle_plugin(gui_ref: Any, plugin_id: str, enabled: bool) -> Dict[str, Any]:
    pm = getattr(gui_ref, '_plugin_manager', None)
    if not pm:
        return {"error": "Plugin manager not available"}
    try:
        toggle = getattr(pm, 'set_enabled', None) or getattr(pm, 'toggle_plugin', None)
        if callable(toggle):
            toggle(plugin_id, enabled)
            return {"ok": True, "plugin_id": plugin_id, "enabled": enabled}
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "Toggle not available"}


def _get_system_info(gui_ref: Any) -> Dict[str, Any]:
    try:
        from config import APP_VERSION
    except ImportError:
        APP_VERSION = "?"
    s = getattr(gui_ref, 'settings', None)
    return {
        "version": APP_VERSION,
        "ui_mode": s.get("ui_mode", "?") if s else "?",
        "data_source": s.get("data_source", "?") if s else "?",
        "mem_data_source": s.get("mem_data_source", "?") if s else "?",
        "uptime_s": round(time.monotonic() - getattr(gui_ref, '_start_time', time.monotonic()), 1),
    }


def _eval_python(gui_ref: Any, expression: str) -> Dict[str, Any]:
    ns = {"gui": gui_ref, "json": json, "time": time}
    try:
        result = eval(expression, {"__builtins__": __builtins__}, ns)
        return {"result": result}
    except Exception as exc:
        return {"error": str(exc)}


def _exec_python(gui_ref: Any, code: str) -> Dict[str, Any]:
    ns = {"gui": gui_ref, "json": json, "time": time, "_output": []}
    wrapped = code + "\n"
    try:
        exec(wrapped, {"__builtins__": __builtins__}, ns)
        return {"ok": True, "output": ns.get("_output", [])}
    except Exception as exc:
        return {"error": str(exc)}
