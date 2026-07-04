# SAO Auto Agent Instructions

## Scope

- This file applies to `sao_auto` only.
- Keep concise. No rollout logs, task transcripts, or debugging history.
- Repository-specific history belongs in `/memories/repo/`.

## Architecture — Platform vs Plugin

SAO ACT UI is a **game-agnostic platform**. All game-specific logic lives in plugins.

**Platform** (`python/` root modules):
- `act_platform/` — Plugin SDK, event bus, declarative UI spec, render hooks
- `engines/` — DPS tracker, encounter manager, trigger engine (game-agnostic interfaces)
- `gui_modules/` — SAO menu, NerveGear button, fisheye, panels, overlays
- `mem_probe/` — Generic memory scanner infrastructure
- `config.py`, `main.py` — Settings, entry point
- `ai_editor/` — Standalone AI IDE (see section below)

**Star Resonance plugin** (`plugins/star_resonance_plugin/`) — the reference game adapter:
- TCP packet capture (Npcap) + IL2CPP memory reading dual data sources
- Boss HP/break/shield, raid mechanic alerts (TTS + banner + directional dodge)
- Buff/debuff monitoring, skill cooldown tracking, auto-key automation
- DPS/HP/Alert/SkillFX/BuffMon overlay panels
- Game-specific menu categories

Other plugins (`hide_seek_plugin/`, `midi_piano_plugin/`) follow the same SDK pattern.

**Rule: platform code never imports plugin code.** Plugins register capabilities through the SDK at runtime.

## Before Working

- Check `/memories/repo/` for task-specific notes.
- Confirm the target surface before editing UI:
  - Entity UI: `sao_gui.py`, `gui_modules/`, `sao_theme/`, Tk/GPU modules
  - WebView UI: `sao_webview.py`, `web/*.html`
  - AI Editor: `ai_editor/`, `web/ai_editor_app.html`
- Default to UTF-8 for all reads/writes.

## Branch And Build

- Current branch: `5.0.0`.
- Do not run `build_release.bat`, PyInstaller, Cython, `dev_publish`, or package publishing unless explicitly requested.
- Stage only intended files; this repo often has unrelated local edits from concurrent sessions.

### Packaging maintenance (XiaoACTUI.spec + dev_publish.py)

**When adding a new package (directory with `__init__.py`):**
1. Add `collect_submodules('package_name')` to `REORG_PKG_HIDDENIMPORTS` in `XiaoACTUI.spec`.
2. Decide delta vs rebuild: if the package contains sensitive/binary code (like `mem_probe/`, `license/`), add it to `IGNORE_PREFIXES` in `dev_publish.py` so changes only go via full runtime-rebuild, not delta.

**Current package routing:**

| Package | spec | dev_publish | Route |
|---------|------|-------------|-------|
| `act_platform/` `act_replay/` `engines/` `gui_modules/` `render/` `sao_theme/` `ui_gpu/` `updater/` `utils/` `ai_editor/` | collect_submodules | delta | Normal: delta .pyc |
| `mem_probe/` | curated 9-module list | IGNORE | Sensitive: full rebuild only |
| `license/` | collect_submodules | IGNORE | Sensitive: full rebuild only |
| `license_server/` | ❌ not in spec | IGNORE | Dev-only: never published |

**When modifying files in an IGNORE-routed package** (`mem_probe/`, `license/`): changes will NOT appear in delta updates. Must do a full runtime-rebuild (`build_release.bat`).

**Plugin tree filtering** (dev_publish.py, line 493+): Files under `plugins/` go as `CAT_DATA` except:
- `il2cpp/out/` and `il2cpp/bin/` (dumper output, 1.8GB+)
- `.dll`, `.exe`, `.pdb` files
- Dev scripts: `diag_dump`, `dump_tool`, `setup_dumper`, `mem_dump_metadata`

## Validation

- `python -m py_compile <changed .py files>` for syntax.
- Final AI Editor `selftest` must be run by the top-level/main agent; Goal Autopilot or other subagents may run focused checks, but their runs do not replace the final main-agent `python -m ai_editor.selftest`.
- `git diff --check -- <changed files>` for whitespace.
- For UI/performance: code checks verify correctness, not behavior. Note when live validation is needed.

## UI And Performance

- Preserve SAO visual style, 60 FPS target.
- Optimize via caching, dirty signatures, throttled refreshes, GPU paths.
- Tk panels: avoid high-frequency `destroy()`/recreate; use dirty signatures.
- DXGI objects are thread-affine.
- Entity alerts use `sao_gui_alert.AlertOverlay`.

### Render / Overlay Compositor — read before touching `render/*`

Before editing anything under `render/` (or `ui_gpu/popup.py`, `gui_modules/sao_gui_fisheye_mixin.py`'s
overlay code), read `.vscode/handoff/overlay-compositor-architecture-notes-2026-07-04.md` in full.
This subsystem has a dense history of subtle, already-diagnosed bugs (deadlocks, GDI/GL handle
leaks, thread-affinity violations, ctypes argtypes pitfalls) — the handoff explains the "why"
behind non-obvious code shapes so you don't re-break something already fixed. It's organized into
numbered sections covering the compositor architecture, z-order/TOPMOST rules, click-through
region sync, GDI/GL resource lifecycle, and verification methodology — match against what you're
about to touch rather than skimming just the title.

## Known Contracts

- Entity menu and webview menu are distinct surfaces — don't port changes unless parity requested.
- HP group fade: keep `HP cover + XT box + STA + HP-bar FX` together, ID plate separate.
- Hide & Seek step order is user-confirmed — don't change without explicit request.

## Memory And Probe

- Runtime addresses are session-sensitive. Treat historical values as clues.
- Read `/memories/repo/` notes before probe work.

## AI Editor (`python/ai_editor/`)

Standalone pywebview IDE with VSCode layout, multi-provider LLM chat, dynamic Chat Provider tabs (Assistant / Claude Code / Codex / plugin), custom Agents & Workflows, three-scope system, and full endpoint customization. See `docs/AI_EDITOR.md` for full architecture reference.

### Architecture

- VSCode layout: Activity Bar → Left Sidebar → Editor + Terminal → **Right Sidebar (Assistant)**
- Three-scope system: System (`~/.sao/`) → Workspace (`<BASE_DIR>/.sao/`) → Plugin (`plugins/<id>/.sao/`)
- Three modes: Agent (autonomous) / Ask (read-only Q&A) / Plan (read + confirm writes)
- Provider registry: plugins call `register_chat_provider()` to add right-sidebar tabs
- Model context windows: built-in 20+ model table + user custom overrides in settings
- Auto-compress: threshold = 90% of model max_input_tokens (VSCode Copilot pattern)

### Key rules

- Tools stay generic. All engine queries go through `engine(action=...)`.
- Agent/Workflow/Instructions load from all three scopes (system → workspace → plugin).
- `ProviderConfig` has full sampling params (top_p, penalties, stop, extra_body, extra_headers, timeout). All customizable from settings UI.
- Right-sidebar provider tabs appear only when their API key is configured (dynamic detection).
- History saved per workspace scope (`<BASE_DIR>/.sao/chat_history/`).
- `SYSTEM_PROMPT` auto-injects: Custom Instructions + Available Agents + Available Workflows.

### Files

| File | Responsibility |
|------|---------------|
| `prompts.py` | System prompts + three-scope instruction loading |
| `llm_engine.py` | Multi-provider LLM, model context window table, full sampling params |
| `tool_registry.py` | Tool registration and dispatch |
| `engine_tools.py` | 12 VSCode-aligned tools + engine aggregate |
| `chat_state.py` | Conversation, tool loop, @-mentions, agent mode, auto-compress |
| `app.py` | pywebview launcher + 60+ JS API methods |
| `agents.py` | Agent registry: 5 built-in + custom from `.sao/agents/` |
| `workflows.py` | Workflow engine: 3 built-in + custom, chain LLM calls with `{{var}}` |
| `scopes.py` | Three-scope resolution + mode/permission defaults |
| `chat_providers.py` | Right-sidebar provider tabs: Assistant/CC/Codex/plugin |
| `mcp_client.py` | MCP server connections (stdio + SSE + internal) |
| `extensions.py` | VSCode Marketplace API client |
| `history.py` | Scope-aware conversation persistence |
| `selftest.py` | 304-item self-test suite |

## Documentation Hygiene

- Keep `AGENTS.md` focused on durable instructions.
- Design notes → docs. Task handoffs → `.vscode/handoff/`. Findings → `/memories/repo/`.
