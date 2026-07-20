# SAO Auto Agent Instructions

## Python Environment
- Use `E:\Py\python.exe` for all Python scripts, tools, checks, and tests in this repository.
- Install Python packages with `E:\Py\python.exe -m pip ...` so they are installed for the correct interpreter.
- Do not substitute another Python installation unless the user explicitly requests it.

## Scope

- This file applies to `sao_auto` only.
- Keep concise. No rollout logs, task transcripts, or debugging history.
- Repository-specific history belongs in `/memories/repo/`.

## Active Platform And Legacy Freeze

- The active platform is native C++ under `C/`.
- Python under `python/` is legacy and temporarily frozen. Do not update, maintain, refactor, or add features to it unless the user explicitly requests work on the legacy Python implementation.
- Default all new platform work, fixes, tests, and documentation to the C++ implementation.
- This freeze overrides the historical Python maintenance guidance below wherever the two conflict.

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

### 死规定 — 双向平台隔离 (Python ↔ C++)

The AI Editor / SaoAiEditor / gpu_hunt stack is written in native C++ (`sao_auto/C/plugins/ai_editor/`). It is a **separate runtime**, not a Python plugin. Bidirectional isolation is mandatory:

1. **C++ side 不 import Python.** No native code under `sao_auto/C/` may pull in `Python.h`, `pybind11`, `cffi`, or reach into the platform Python runtime. The only sanctioned bridge is `plugins/python_host/` (which is a Python-loaded shim, not a C++ import).
2. **Python 主 UI 不 launcher C++ 二进制.** `sao_auto/python/gui_modules/*`, `sao_gui_*`, and any platform-level Python code MUST NOT `subprocess.Popen` / `os.startfile` / os-exec SaoAiEditor.exe (or any other native binary) to open a C++ panel. That would embed a build-tree assumption into the Python UI and make the "platform never imports plugin" rule leak sideways through file-path knowledge. Do not add "Open GPU Hunt" / "Open AI Editor Native Panel" / etc. buttons in the platform menu that shell out to a native exe.
3. C++ panels / windows / trays are launched by the **C++ binary itself** — via its own CLI flags, its own tray icon, a Windows shortcut, or (when it acts as a plugin) via the plugin SDK's declarative UI registration path. The Python host is unaware of the C++ binary's on-disk location.
4. Violation of this rule is a hard revert. If a native panel needs a menu entry inside the Python UI, ship it as a real plugin through the SDK — do not smuggle in a Popen call.


## Before Working

- Check `/memories/repo/` for task-specific notes.
- Confirm the target surface before editing UI:
  - Entity UI: `sao_gui.py`, `gui_modules/`, `sao_theme/`, Tk/GPU modules
  - AI Editor: `ai_editor/`, `web/ai_editor_app.html`
  - **WebView UI: paused — do not maintain.** `sao_webview.py` + `web/menu.html` / `web/act_aggregate.html` / other legacy `web/*.html` (excluding AI Editor) are frozen. Do NOT invest effort in:
    - New features / panels / fields (overrides the historical "double-UI parity" rule — Entity-only for new work)
    - Cleaning up historical coupling leaks inside `sao_webview.py` (hardcoded plugin attributes: `_recognition_engines`, `_packet_engine`, `_vision_engine`, `_recognition_active`, `_vision_paused_for_death`, etc. — leave them as-is, do NOT extract or refactor)
    - Adding new tests / selftests targeting `sao_webview.py`
    - Existing production paths through `sao_webview.py` are untouched; only maintenance work on it is off-limits unless a blocking runtime bug forces it.
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

## Git — test files never committed

- **Test files MUST NOT be added to git.** This includes all `*selftest*.py`, `*_test*.py`, `test_*.py`, conftest.py, and any file under `tools/` that is a selftest/test harness.
- Do not `git add` any test file. Do not commit test files alongside feature/fix changes.
- If a test file is untracked, leave it untracked. If a test file is already tracked, do not stage further changes to it.
- Rationale: test files are local-only development artifacts; they must not ship to the repository or downstream consumers.
- When committing a batch, explicitly exclude test files:
  ```
  git add <non-test files only>
  # never: git add -A  (would include test files)
  ```

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

## Comment Style — hard rule (docstrings banned outside AI Editor)

- **All comments MUST use `#` line comments.** Docstrings (`"""..."""` or `'''..."""`) as comments/docs are **banned** everywhere in this repo EXCEPT the three AI Editor packages: `python/ai_editor/`, `python/ai_editor_extensions/`, `python/ai_editor_history/`.
- Applies to modules, classes, functions, and inline blocks. No exceptions for "brief one-liners".
- Multi-line strings used as actual string literals (assigned, passed as args, returned) are fine — they are not docstrings.
- If you encounter a docstring outside AI Editor, convert it to `#` comments in the same change. Do NOT reintroduce one.
- Rationale: uniform comment style + explicit user policy. `__doc__` inflation and mixed comment/reflection semantics were the recurring complaints.

## Wave-Named Persistent Artifacts — Hard Ban

**持久文件、目录、CMake target、C ABI 符号、测试 tag 一律不得用 `wave*` 命名。**

`wave` 是过去的迭代批次代号，不是功能名。新代码必须用**功能名**命名，例如：
- `sao_engine_event_bus_*_wave5` → `sao_engine_event_bus_*_priority`（priority + cancel 语义）
- `sao_wave17c_support_lib` → `sao_core_gap_support_lib`（gap closure 支持）
- `sao_wave17a_ui_support_lib` → `sao_ui_gap_support_lib`
- `test_*_wave*.cpp` → `test_*_<功能>.cpp`（如 `test_event_bus_priority.cpp`）
- `[wave5]` / `[wave18a]` 等 Catch2 tag → `[priority]` / `[sandbox]` 等功能 tag

**例外**：
- 已发布的 C ABI 导出符号（如 `sao_shell_crypter_wave8_*`、`sao_security_anti_debug_wave8_*`）保留 wave 标签作为二进制 ABI 版本标识，避免破坏 DLL 二进制兼容。
- 历史 changelog 行项中 "Wave N" 字样作为历史记录保留（PLAN.md/STATUS.md/CHANGELOG.md 历史表格）。

**新代码违反 = broken change**。CI 的 `sao_assert_no_wave_impl_libraries` 函数已禁止 `wave*` 命名的临时 impl 库；新 target/文件/符号同样不得以 wave 开头。

## Documentation Hygiene

- Keep `AGENTS.md` focused on durable instructions.
- Design notes → docs. Task handoffs → `.vscode/handoff/`. Findings → `/memories/repo/`.
