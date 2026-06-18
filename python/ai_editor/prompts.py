"""System prompts for the SAO AI Editor.

Architecture follows VSCode Copilot's prompt composition:
  1. Role identity + capabilities
  2. Tool rules (per-tool, conditional)
  3. Domain knowledge (mem_probe, engine, plugin SDK)
  4. Safety guardrails
  5. Project structure

Update this file whenever tools or project scope change.
See AGENTS.md "System prompt maintenance" section.
"""

from __future__ import annotations

import base64
import hashlib
import os
import zlib


# ---------------------------------------------------------------------------
# Encrypted premium guide (Engine A/B/C/D documentation)
# ---------------------------------------------------------------------------

_ENCRYPTED_ENGINE_GUIDE = (
    "W<xi0<oozq1WjG~+w&93`4YI7!`8g8Rq*7x3)-QUt?KWIq(H)-_;JPFF|vQH8q-##Yyt=0F#p5|h"
    "le!b^vzG>T7j1-MGBx)+<#_%TJAPfX2>r+cdX8)I7+*{LlMSilV4Vd_ta6q+~gxy5OBn)qji+lP`"
    "m&fN(&7oPaJNAD<#{=4Ql!JLdE<te(tG<1KLix7EV3qa_&Vy*)=z(0#>u!iuzI+3_<T)v=$45nP2"
    "$C6-T6)G2KAIG|Hr+03B{SSXhSLXOYMPSYBq|dXmn+RbJ;<czYDGTWcm3w4k#u_2W*%Ez0B<qet6"
    "8ouvkPsn7$Ns`^w!o2qbuc%Xvsvf|%?XDrzcA>rpq4AlH>x@MF}^;8!R&d2jMP$SI({g1rO*<g%j"
    "Me_tbE+)wgVFUN}n%wLE1qI7eJdF84cLk-dt&l5kfFF*uo495_4BVzCAH-q%f|DQqPXtl-S4~@pC"
    "Q8LuR|P7(Igw|wY9M66BsE<~=nqd2(2w!v{=eFhw{k1Lv{Yjdd#0D}>g)I}N+gEYt&bnXjW_lrPY"
    "o#Q4R{hPg-Ph<lTo^wYucsAccKlA{0|;8=)u^m;*O3{3hb#G!VBtnn{K)p>e;)r;>t1ilkaG8h9W"
    "i<2;c#tk=kkkL4FD_V~~UHlyp!(zZsc0lUJj9i7@EE4fhSdl8ELa8bMip(}EobSH@NP<YV$-X@we"
    "*@I%;q12RP3uf$Y7znnEe7{5Rq3NQ8S4rwX^B%~b{U}B+B_*l#HvQCZ9&v(;9c<MZ1QeZ;r$sW~6"
    "MmNr(PwQDjpdH1r&z4jc7^atFKX?ww<SOsl>8Nspdwd!rmY&CslvA2uVItn>$uSDODQ5B}x-&@sV"
    "mXmSxC}`MdHu}jt>(Q&iry*7hvL6@doBB4F_p})RtW_EagOY;BF9c7<N#s$%NN-2)^Pa<Bpc+d5>"
    "q_JI{t-u`Enc&VKGYHo9ym5wf!u<=HvVnt8*hKN%p|kQ}IyEV1zu|c#oe8N(<#aT@MkKWfua+-cS"
    "zJOY2xwbjW?E)SBlJKJ_mh-Fc9`s->!}5zA&2Y5?0K%CEd&ipO!u!Ea)=pjwUmTO=O#kL5w4ma?d"
    "tP%#D<VUh|na>yyjJ2MJu{?LuG7k4sSH#^sk;|$}BkN05@V!08&Q#LrTQg(j>+tY~Nj68$@*?vXw"
    "yqRaC?~5|H52#g^0QYS3SLuOYmNMax;fPgA06S_nUC6_rjraI<qKoF+p-ySO7A4M3H*U7ygM6VMe"
    "=D$Xhs+-8LpH6XKE5qi&(*TQ!|#Cj^00^fWu8z`?5ama%VMg5bIO_riNOb<d%t0F2}XWj4X+{h19"
    "~$#EP%2{*mjpEu^vZWzjrC-pmbWlD^A5?S>b3LT1Zkq@V>ZYJ(b*-8;pzSwm3zR!C(5>NMkmOX6~"
    "2j#Y{7vZ79Ed8Kklh&-R5m5(fB|z3wqb#~0BWE9cI#<y119Sv?0nTArxOLt9R~O#-)Dpl}9$$`ll"
    "w$K%VB1l6+R6UHx>#PitL)W(Eb+1qY^EfkeE0*r#}nEIKZkCEyfRLte7KtKUw@eA+fDB2O_C%#q-"
    "U<U^!ENv!eK$uei<AsdyGQmSGeic;$V;-#JY4tcEA#h1a^QxiE`+el)hn`-1_;@%we(g*H>PniMU"
    "K*bt5GiNYF*N;SrVFd8)2#F^!ce}-!(%feDE9Ej|M?CuK8BqQGFyTPnzqoC;6M5?@mY_KMFKb5S4"
    "wLf!`ol%%nfMLK62|amv!j&<mYU0Z{UBjxPZ0w72bjPBQc)4!3$Gcm|!XKQsst@HrRS2gTN?n5M&"
    "iYpK!6h$z^)DaT`T5LNT#ex2mS_ROJo#Ll>v-{D|VIWq*YO(z3KSw_Vldr*=q*;_%Dc=F_MH8Nbz"
    "d2`&Y7vr_ru(asMJq!ElK9Sq?Uuwgs~Mt_(XOo2oNSv-~HB2}^rzw0h#PWs0Sp;(_>pSX?VZCgGT"
    "9#&K`B=d0Nv)@e0lx%shM?ha!@pS=nwR<HJA#K1mf$s1f)hdaQLboM=o)^*W#y>?ualH^$g||Uue"
    "t>56&!t?nD4?{|;jc=l+o#wu=?()3I(B-E@f%mIU{+Rn;;2T?lxaI6Q3rRe;D!V"
)

_EG_SEED = b"\x4a\x91\xc3\x7f\x28\xe5\xd6\x0b\x73\xfa\x14\x9d\x55\xa2\x68\xbe"


def _decrypt_engine_guide() -> str:
    k = hashlib.sha256(_EG_SEED).digest()
    raw = base64.b85decode(_ENCRYPTED_ENGINE_GUIDE)
    dec = bytearray(len(raw))
    kl = len(k)
    for i, b in enumerate(raw):
        dec[i] = b ^ k[i % kl]
    return zlib.decompress(bytes(dec)).decode("utf-8")


def _check_paid() -> bool:
    try:
        from license import get_license_manager
        return get_license_manager().is_paid
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Building blocks — composed into the final prompt
# ---------------------------------------------------------------------------

_IDENTITY = """\
You are a highly sophisticated automated coding agent for **SAO ACT UI** \
(v5.0.0), a game-agnostic combat analysis platform with SAO-style overlay, \
plugin SDK, and extensible automation.

You have expert knowledge of Python, JavaScript/HTML/CSS, Win32, memory \
scanning, and real-time data processing. The user will ask questions or \
request tasks — you answer accurately and use tools to accomplish work.\
"""

_TOOL_RULES = """\

## Tool Rules

If you think running multiple tools can answer the user's question, \
prefer calling them **in parallel** when they are independent.

When using a tool, follow the JSON schema carefully and include ALL \
required properties. Do not ask permission before using a tool — just \
use it. NEVER say the name of a tool to the user (e.g., don't say \
"I'll use readFile"); just perform the action and present the result.

### File tools

- **readFile(path, startLine?, endLine?)** — Read file contents. \
  Prefer reading a large section over calling multiple times for \
  small pieces of the same file.
- **editFile(path, content, startLine?, endLine?)** — Create or edit \
  a file. **Always readFile first** to see current content before \
  editing. Use startLine/endLine for surgical edits; omit them for \
  full file rewrites. Include 2-3 lines of unchanged context around \
  your changes so the edit lands correctly.
- **listFiles(path, pattern?, recursive?)** — List directory contents. \
  Use pattern="*.py" to filter. Use recursive=true to search deeply.
- **searchFiles(query, path?, pattern?, regex?, caseSensitive?)** — \
  Grep across files. Use regex=true for patterns. Always specify \
  path to narrow scope.

### Terminal

- **runTerminal(command, cwd?)** — Execute a shell command (30s timeout). \
  Use for: running tests, installing packages, git operations, \
  compilation, any CLI task. Check exit code in the result.

### Interaction

- **askQuestion(question)** — Ask the user when you need clarification. \
  Don't ask unnecessary questions — if you can figure it out from \
  context or by reading files, do that instead.
- **taskComplete(summary)** — Signal that the current task is finished. \
  Include a brief summary of what was accomplished.
- **getConfirmation(action, risk?)** — Ask confirmation before \
  dangerous operations (deleting files, modifying settings, running \
  destructive commands). Explain what will happen.

### Editor

- **editor_getContent()** — Read the active editor tab's content.
- **editor_setContent(content, language?)** — Write to the editor tab.
- **editor_getSelection()** — Get the currently selected text.

### Engine (runtime)

- **engine(action, ...)** — Single entry point for all runtime queries.

  **Platform actions** (always available):
  - `engine(action="system_info")` — Version, uptime, UI mode
  - `engine(action="plugins")` — List installed plugins
  - `engine(action="settings_get", key="...")` — Read a setting
  - `engine(action="settings_set", key="...", value=...)` — Write a setting
  - `engine(action="memory_status")` — Memory data source health
  - `engine(action="eval", expression="...")` — Evaluate Python expression \
    in the running process (has access to `gui` object)
  - `engine(action="exec", code="...")` — Execute Python code block \
    (use `_output.append(...)` to return data)

  **Plugin actions** are registered dynamically by loaded plugins. Use \
  `engine(action="plugins")` to inspect what is loaded, then rely on \
  plugin documentation for plugin-specific action names.
"""

_MEM_PROBE_GUIDE = """\

## Memory Scanner (mem_probe/)

The platform includes a generic memory scanning infrastructure. Key modules:

- **mem_probe.process.GameProcess** — Attaches to a target process \
  (process names supplied by the active plugin). Requires admin. Provides \
  `read_bytes(addr, size)`, `read_uint32/64(addr)`, `modules()`, \
  `memory_regions()`.
- **mem_probe.scanner** — Multi-frame value search. `scan(process, value)` → \
  candidate addresses, `narrow(process, candidates, new_value)` → refined set.
- **mem_probe.cy_memscan** — AVX2-accelerated scanning (Cython). Falls back \
  to pure Python if the extension isn't built.
- **mem_probe.unified_source** — TCP/memory hybrid data source bridge. \
  Plugin-specific bridges are injected by plugins via \
  `set_bridge_classes(StateBridgeCls, SelfStateProviderCls)`.

### Using mem_probe via engine tool

```
engine(action="memory_status")
```
Returns connection state, reader type, and health metrics.

### Plugin memory bridges

Plugins may register domain-specific memory bridges at load time. Platform \
code does not import those bridges directly.\
"""

_SAFETY = """\

## Working Style

- Answer in the user's language (Chinese or English).
- Use markdown with code blocks for code output.
- **Never guess** file contents or runtime state — always use tools.
- **Read before edit** — always `readFile` before `editFile`.
- **Verify after edit** — run `runTerminal` to test/compile.
- For **multi-step tasks**: read → plan → edit → verify → report.
- Platform code is **game-agnostic**. Plugin code stays inside its \
  own `plugins/<name>/` directory.
- Do not expose internal tool names to the user.
- If you're unsure about a destructive operation, use `getConfirmation`.
- Memory scanning requires admin. If process attach fails, suggest \
  the user run as administrator.\
"""

_PROJECT_STRUCTURE = """\

## Project Structure

```
sao_auto/python/
├── config.py              — Settings and version
├── main.py                — Entry point
├── act_platform/          — Plugin SDK, event bus, UI spec
├── engines/               — Platform runtime engines
├── gui_modules/           — SAO menu, panels, overlays (Entity/Tk)
├── sao_webview.py         — WebView overlay host
├── mem_probe/             — Memory scanner (process, scanner, hybrid source)
│   └── unified_source.py  — TCP/memory hybrid (bridges injected by plugins)
├── plugins/
│   └── <plugin_id>/       — Plugin-owned runtime, UI, and data adapters
├── web/                   — HTML/CSS/JS for WebView + AI Editor
├── ai_editor/             — This AI editor backend
└── license/               — Auth system
```
"""

# ---------------------------------------------------------------------------
# Compose
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = _IDENTITY + _TOOL_RULES + _MEM_PROBE_GUIDE + _SAFETY + _PROJECT_STRUCTURE

# Kept as named constant for AGENTS.md reference; no "Safety" section.

AGENT_MODE_ADDITION = """\

## Agent Mode Active

You are now in autonomous agent mode. Work through the task independently:

1. **Understand** — Read relevant files and gather context with tools.
2. **Plan** — State your approach in 2-3 sentences. Don't over-plan.
3. **Execute** — Make changes. Read files before editing. Run tests after.
4. **Iterate** — If tests fail or something looks wrong, fix it immediately.
5. **Complete** — Use `taskComplete(summary)` when done.

Prefer calling multiple tools in parallel when possible. Don't ask \
permission for each step — just do it. Only use `askQuestion` when \
genuinely blocked on a decision the user must make.
"""


def _resolve_base_dir() -> str:
    try:
        from config import BASE_DIR
        return BASE_DIR
    except Exception:
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load_instructions_from_dir(base: str, label: str = "") -> list[str]:
    """Collect instructions from a single scope directory."""
    parts: list[str] = []
    proj_file = os.path.join(base, "instructions.md")
    if os.path.isfile(proj_file):
        try:
            with open(proj_file, "r", encoding="utf-8") as f:
                body = f.read().strip()
            if body:
                parts.append(body)
        except Exception:
            pass
    inst_dir = os.path.join(base, "instructions")
    if os.path.isdir(inst_dir):
        for fname in sorted(os.listdir(inst_dir)):
            if not fname.endswith(".md"):
                continue
            fpath = os.path.join(inst_dir, fname)
            if not os.path.isfile(fpath):
                continue
            try:
                with open(fpath, "r", encoding="utf-8") as f:
                    body = f.read().strip()
                if body:
                    parts.append(f"### {fname[:-3]}\n\n{body}")
            except Exception:
                pass
    return parts


def _read_markdown_file(path: str, heading: str = "") -> list[str]:
    try:
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8") as f:
                body = f.read().strip()
            if body:
                return [f"### {heading}\n\n{body}" if heading else body]
    except Exception:
        pass
    return []


def _load_custom_location(path: str, root: str) -> list[str]:
    if not path:
        return []
    target = path if os.path.isabs(path) else os.path.join(root, path)
    if os.path.isdir(target):
        parts: list[str] = []
        for fname in sorted(os.listdir(target)):
            if fname.endswith(".md"):
                parts.extend(_read_markdown_file(os.path.join(target, fname), fname[:-3]))
        return parts
    return _read_markdown_file(target, os.path.basename(target))


def _customization_settings(settings_getter=None) -> dict:
    if not settings_getter:
        return {}
    try:
        ai = settings_getter("ai_editor", {}) or {}
    except Exception:
        return {}
    customization = ai.get("customization", {}) if isinstance(ai, dict) else {}
    return customization if isinstance(customization, dict) else {}


def load_instructions(settings_getter=None, workspace_root: str = "") -> str:
    """Collect custom instructions from all scopes.

    Sources (all appended):
      1. User instructions from settings
      2. System scope  ``~/.sao/``
      3. Workspace scope ``<BASE_DIR>/.sao/``
      4. Plugin scopes ``plugins/<id>/.sao/``
    """
    parts: list[str] = []

    # User-level from settings
    customization = _customization_settings(settings_getter)
    use_agent_md = customization.get("use_agent_md", True) is not False
    use_claude_md = customization.get("use_claude_md", False) is True
    extra_locations = customization.get("instructions_locations", [])
    if not isinstance(extra_locations, list):
        extra_locations = []
    if settings_getter:
        ai = settings_getter("ai_editor", {}) or {}
        if isinstance(ai, dict):
            text = ai.get("user_instructions", "")
        else:
            text = ""
        if text and text.strip():
            parts.append(text.strip())

    # All scopes
    if workspace_root:
        scope_parts = _load_instructions_from_dir(
            os.path.join(workspace_root, ".sao"), "Workspace")
        parts.extend(scope_parts)
    else:
        try:
            from ai_editor.scopes import resolve_scopes
            for entry in resolve_scopes():
                scope_root = os.path.dirname(entry["path"]) if os.path.basename(entry["path"]) == ".sao" else entry["path"]
                scope_parts = _load_instructions_from_dir(
                    entry["path"], entry.get("label", ""))
                if use_agent_md:
                    scope_parts.extend(_read_markdown_file(
                        os.path.join(scope_root, "AGENTS.md"), "AGENTS.md"))
                if use_claude_md:
                    scope_parts.extend(_read_markdown_file(
                        os.path.join(scope_root, "CLAUDE.md"), "CLAUDE.md"))
                if entry.get("scope") == "workspace":
                    for loc in extra_locations:
                        norm = str(loc).replace("\\", "/").strip("/")
                        if norm in {".sao/instructions.md", ".sao/instructions"}:
                            continue
                        scope_parts.extend(_load_custom_location(str(loc), scope_root))
                if scope_parts:
                    parts.append(f"<!-- scope: {entry.get('label','')} -->")
                    parts.extend(scope_parts)
        except Exception:
            root = _resolve_base_dir()
            parts.extend(_load_instructions_from_dir(
                os.path.join(root, ".sao")))

    if not parts:
        return ""
    return "\n\n# Custom Instructions\n\n" + "\n\n---\n\n".join(parts)


def _list_files_in_scope(sao_dir: str, scope_label: str) -> list[dict]:
    files: list[dict] = []
    proj_file = os.path.join(sao_dir, "instructions.md")
    if os.path.isfile(proj_file):
        try:
            with open(proj_file, "r", encoding="utf-8") as f:
                body = f.read()
            files.append({"name": "instructions.md", "path": proj_file,
                          "scope": scope_label, "content": body})
        except Exception:
            pass
    inst_dir = os.path.join(sao_dir, "instructions")
    if os.path.isdir(inst_dir):
        for fname in sorted(os.listdir(inst_dir)):
            if not fname.endswith(".md"):
                continue
            fpath = os.path.join(inst_dir, fname)
            if not os.path.isfile(fpath):
                continue
            try:
                with open(fpath, "r", encoding="utf-8") as f:
                    body = f.read()
                files.append({"name": fname, "path": fpath,
                              "scope": scope_label, "content": body})
            except Exception:
                pass
    return files


def list_instruction_files(workspace_root: str = "") -> list[dict]:
    """Return metadata for instruction files across all scopes."""
    if workspace_root:
        return _list_files_in_scope(
            os.path.join(workspace_root, ".sao"), "workspace")
    files: list[dict] = []
    try:
        from ai_editor.scopes import resolve_scopes
        for entry in resolve_scopes():
            files.extend(_list_files_in_scope(
                entry["path"], entry.get("label", entry["scope"])))
    except Exception:
        root = _resolve_base_dir()
        files.extend(_list_files_in_scope(
            os.path.join(root, ".sao"), "workspace"))
    return files


def save_instruction_file(name: str, content: str,
                          workspace_root: str = "") -> dict:
    """Create or update an instruction file. Returns ``{ok, path}``."""
    root = workspace_root or _resolve_base_dir()
    if name == "instructions.md":
        target = os.path.join(root, ".sao", "instructions.md")
    else:
        if not name.endswith(".md"):
            name += ".md"
        target = os.path.join(root, ".sao", "instructions", name)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "w", encoding="utf-8") as f:
        f.write(content)
    return {"ok": True, "path": target}


def delete_instruction_file(name: str,
                            workspace_root: str = "") -> dict:
    """Delete an instruction file. Returns ``{ok}``."""
    root = workspace_root or _resolve_base_dir()
    if name == "instructions.md":
        target = os.path.join(root, ".sao", "instructions.md")
    else:
        target = os.path.join(root, ".sao", "instructions", name)
    if os.path.isfile(target):
        os.remove(target)
        return {"ok": True}
    return {"ok": False, "error": "File not found"}


# Cache for agent/workflow prompt sections — rebuild only when registry changes
_agent_prompt_cache: str = ""
_agent_prompt_version: int = -1
_workflow_prompt_cache: str = ""
_workflow_prompt_version: int = -1


def get_system_prompt(agent_mode: bool = False, custom: str = "",
                      settings_getter=None) -> str:
    """Build the system prompt for a conversation."""
    global _agent_prompt_cache, _agent_prompt_version
    global _workflow_prompt_cache, _workflow_prompt_version

    if custom:
        return custom
    prompt = SYSTEM_PROMPT
    if _check_paid():
        try:
            prompt += _decrypt_engine_guide()
        except Exception:
            pass
    inst = load_instructions(settings_getter=settings_getter)
    if inst:
        prompt += "\n" + inst
    try:
        from ai_editor.agents import get_agent_registry
        reg = get_agent_registry()
        ver = getattr(reg, '_version', 0)
        if ver != _agent_prompt_version:
            _agent_prompt_cache = reg.to_prompt_section()
            _agent_prompt_version = ver
        prompt += _agent_prompt_cache
    except Exception:
        pass
    try:
        from ai_editor.workflows import get_workflow_registry
        reg = get_workflow_registry()
        ver = getattr(reg, '_version', 0)
        if ver != _workflow_prompt_version:
            _workflow_prompt_cache = reg.to_prompt_section()
            _workflow_prompt_version = ver
        prompt += _workflow_prompt_cache
    except Exception:
        pass
    if agent_mode:
        prompt += AGENT_MODE_ADDITION
    return prompt
