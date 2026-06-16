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
- Keep PyInstaller specs aligned with module layout (`hiddenimports`, `collect_submodules`).

## Validation

- `python -m py_compile <changed .py files>` for syntax.
- `git diff --check -- <changed files>` for whitespace.
- For UI/performance: code checks verify correctness, not behavior. Note when live validation is needed.

## UI And Performance

- Preserve SAO visual style, 60 FPS target.
- Optimize via caching, dirty signatures, throttled refreshes, GPU paths.
- Tk panels: avoid high-frequency `destroy()`/recreate; use dirty signatures.
- DXGI objects are thread-affine.
- Entity alerts use `sao_gui_alert.AlertOverlay`.

## Known Contracts

- Entity menu and webview menu are distinct surfaces — don't port changes unless parity requested.
- HP group fade: keep `HP cover + XT box + STA + HP-bar FX` together, ID plate separate.
- Hide & Seek step order is user-confirmed — don't change without explicit request.

## Memory And Probe

- Runtime addresses are session-sensitive. Treat historical values as clues.
- Read `/memories/repo/` notes before probe work.

## AI Editor (`python/ai_editor/`)

Standalone pywebview IDE with multi-provider LLM chat, VSCode-aligned tools, MCP, and extension marketplace.

### Architecture

- **Platform code** — but aware of the Star Resonance plugin as reference example.
- `engine()` tool dispatches to platform engines + whatever plugin is loaded.
- Tools follow VSCode Copilot pattern: `readFile`, `editFile`, `listFiles`, `searchFiles`, `runTerminal`, `askQuestion`, `taskComplete`, `getConfirmation`, editor tools, and the `engine` aggregate.

### System prompt (`prompts.py`)

- `SYSTEM_PROMPT` describes the platform, lists the Star Resonance plugin's specific features as the reference adapter, enumerates tools, and explains IDE usage.
- **When tools change**: update the tool table and engine action list in `SYSTEM_PROMPT`.
- **When project scope changes**: update the "About This Project" section.
- `AGENT_MODE_ADDITION` appended when Agent Mode is checked.
- Both `app.py` (pywebview) and `sao_gui_ai_editor.py` (Tk fallback) call `get_system_prompt()`.

### IDE rules

- Tools stay generic. All engine queries go through `engine(action=...)`.
- HTML UI (`web/ai_editor_app.html`) supports dark/light via `[data-theme]` CSS variables, synced with ACT `panel_themes.act`.
- Extension tools loaded from VSIX `package.json` `contributes`.
- MCP servers loaded from `mcp.json` / settings.

### Files

| File | Responsibility |
|------|---------------|
| `prompts.py` | System prompts — update when tools or scope change |
| `llm_engine.py` | Multi-provider LLM (OpenAI, Anthropic native, compatible) |
| `tool_registry.py` | Tool registration and dispatch |
| `engine_tools.py` | VSCode-aligned tools + engine aggregate |
| `chat_state.py` | Conversation, tool loop, @-mentions, agent mode |
| `app.py` | pywebview launcher + JS API bridge |
| `mcp_client.py` | MCP server connections (stdio + SSE) |
| `extensions.py` | VSCode Marketplace API client + extension tool loading |
| `history.py` | Conversation persistence |
| `selftest.py` | Self-test suite |

## Documentation Hygiene

- Keep `AGENTS.md` focused on durable instructions.
- Design notes → docs. Task handoffs → `.vscode/handoff/`. Findings → `/memories/repo/`.
