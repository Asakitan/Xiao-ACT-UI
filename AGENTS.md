# SAO Auto Agent Instructions

## Scope

- This file applies to `sao_auto` only.
- Keep this file as concise operating instructions. Do not add rollout logs, task transcripts, or historical debugging notes here.
- Repository-specific history belongs in `/memories/repo/`; consult the relevant memory file when a task touches an older subsystem.

## Before Working

- Check `/memories/repo/` for task-specific notes before changing code or answering project questions.
- Inspect local instructions/configuration such as `AGENTS.md`, `.github/instructions/`, `.codex/`, and Copilot instructions when they exist.
- Confirm the active target surface before editing UI code:
  - Entity UI: `sao_gui.py`, `sao_theme.py`, `sao_menu_hud.py`, `sao_gui_hp.py`, related Tk/GPU modules.
  - Webview UI: `sao_webview.py`, `web/*.html`, web panel/menu assets.
- Default to UTF-8 for reads/writes and terminal output on Windows.

## Branch And Refactor Policy

- `sao_auto` refactor work continues on branch `3.0.0` unless the user says otherwise.
- The user has authorized structural refactors in `sao_auto`: split giant files, add modules/packages, and organize code by responsibility.
- When splitting modules, preserve public imports with `__init__.py` re-exports where needed.
- Keep PyInstaller specs aligned with new module layout, especially `hiddenimports` and `collect_submodules` usage.

## Build, Publish, And Git Guardrails

- Do not run `build_release.bat`, PyInstaller, Cython build commands, `dev_publish`, update-server upload commands, or package publishing unless explicitly requested in the current turn.
- In this repo, “远端” / “推送到 GitHub” means git commit/push to `origin`, not release packaging or update-server upload.
- Stage only intended files; this repo often has unrelated local edits.
- Before assuming a push or release landed, check remote state. GitHub auth/pushes have previously failed on this machine.

## Validation

- Prefer targeted validation for touched files:
  - `python -m py_compile <changed .py files>`
  - `git diff --check -- <changed files>`
- Do not use repo-wide `compileall` as the only gate; legacy Python 2 scripts under old tool folders can fail unrelated syntax checks.
- For UI/performance fixes, code checks are not enough when live behavior matters. Add focused runtime/manual verification notes when live gameplay or browser/UI validation is unavailable.

## UI And Performance Preferences

- Preserve SAO visual style, effects, scanlines, animations, and intended 60 FPS behavior unless the user explicitly asks to simplify visuals.
- Optimize by reducing redundant work: caching, dirty signatures, deduplicated overlay pushes, scheduler gating, worker decoupling, and GPU paths where appropriate.
- For Entity/Tk panels, assume CPU drawing can become a bottleneck: throttle refreshes, use dirty signatures before rebuilding widgets, avoid high-frequency `destroy()`/recreate loops, move heavy arithmetic/signature work into existing Cython helpers when it is hot, and prefer WebView/GPU overlay rendering for animated or frequently-updated panels.
- Do not treat “idle looks fine” as proof for performance fixes. Heavy-combat paths, worker backlog, animation gating, capture locks, and visible-panel scheduling are common failure points.
- DXGI desktop duplication objects are thread-affine. Do not share a `DxgiDuplicationSession` across threads.
- Entity reminder/alert popups should use `sao_gui_alert.AlertOverlay` by default.

## Known UI Contracts

- Hide & Seek step order/templates are user-confirmed. Do not change its state machine, step order, or recognition flow unless explicitly requested.
- Entity HP interaction lives in `sao_gui_hp.py`; GPU HP mode is click-through and is not the place for mouse hover/click behavior.
- HP group fade should keep `HP cover + XT box + STA + HP-bar interaction FX` together, with the ID plate separate.
- Popup-style SAO feedback means visible scale, yellow ring/highlight, and SAO sound treatment when applicable.
- Entity menu and webview menu are distinct surfaces; do not port changes between them unless parity is requested.

## Memory And Probe Work

- Runtime memory/probe addresses are environment- and session-sensitive. Treat historical addresses as clues, not stable constants.
- For live name/table/probe work, read the relevant `/memories/repo/` notes first and prefer targeted scripts/tasks over broad scans.

## Documentation Hygiene

- Keep `AGENTS.md` focused on durable instructions.
- Put design notes in docs, task handoffs in rollout/handoff files, and historical findings in `/memories/repo/`.
- If a new lesson is broadly reusable, add a short repo memory note instead of appending a long narrative here.

## AI Editor (python/ai_editor/)

The AI Editor is a standalone pywebview-based IDE window with multi-provider LLM chat, tool calling, and a VSCode Marketplace extension browser. When modifying it:

### Architecture boundaries
- The AI Editor is **platform code**, not plugin code. It must not reference any specific game plugin.
- Game-specific features (DPS, Boss, Buff, Auto-key) are provided by plugins at runtime. The `engine()` tool dispatches to whatever plugin is loaded.
- The tools follow the VSCode Copilot pattern: `readFile`, `editFile`, `listFiles`, `searchFiles`, `runTerminal`, `askQuestion`, `taskComplete`, `getConfirmation`, plus editor tools and the `engine` aggregate.

### System prompt maintenance (python/ai_editor/prompts.py)
- `SYSTEM_PROMPT` introduces the platform (game-agnostic), lists tools, and explains IDE usage.
- Game-specific content belongs in plugin descriptions, not in the system prompt.
- When adding or removing tools, update the tool table in `SYSTEM_PROMPT` and the engine action list.
- `AGENT_MODE_ADDITION` is appended when Agent Mode checkbox is checked.
- Both `app.py` (pywebview) and `sao_gui_ai_editor.py` (Tk fallback) call `get_system_prompt()`.

### IDE requirements
- Tools must align with VSCode Copilot's design. Keep them generic (file/terminal/editor/interaction), not engine-specific.
- All engine queries go through the single `engine(action=...)` aggregate tool.
- The HTML UI (`web/ai_editor_app.html`) supports dark/light theme via `[data-theme]` CSS variable sets. Theme syncs with ACT's `panel_themes.act` setting.
- Extension tools are loaded from installed VSIX `package.json` `contributes` (languageModelTools, chatParticipants, commands).
- MCP servers are loaded from `mcp.json` / settings. MCP tools appear as `mcp_<server>_<tool>` in the tool list.

### File inventory
- `prompts.py` — System prompts (update when tools or project scope changes)
- `llm_engine.py` — Multi-provider LLM (OpenAI, Anthropic native, compatible)
- `tool_registry.py` — Tool registration and dispatch
- `engine_tools.py` — VSCode-aligned tools + engine aggregate
- `chat_state.py` — Conversation management, tool loop, agent mode
- `app.py` — pywebview launcher + JS API bridge
- `mcp_client.py` — MCP server connections (stdio + SSE)
- `extensions.py` — VSCode Marketplace API client
- `history.py` — Conversation persistence
- `selftest.py` — 59-item self-test suite
