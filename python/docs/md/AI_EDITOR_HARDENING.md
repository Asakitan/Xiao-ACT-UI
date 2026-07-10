# AI Editor Hardening and UX Simplification

## Objective

Strengthen the existing AI Editor without replacing its current architecture or
removing supported providers, extensions, tools, workflows, or editor features.
The work is delivered in independently testable slices so each security or UX
change can be reverted without undoing unrelated fixes.

## Threat and failure model

- A downloaded VSIX, workspace MCP configuration, extension Webview, or Node
  extension host is not trusted merely because it is locally configured.
- Tokens, API keys, authorization headers, and MCP environment variables must
  not be persisted as plaintext or returned through broad JS/extension DTOs.
- Cancellation is asynchronous: an old model stream may outlive a five-second
  join and must never mutate a newer conversation or execute newer tools.
- Tool calls and tool results form one protocol unit and must not be separated
  by context compaction.
- The declared 600x400 minimum window and keyboard-only operation are supported
  product states, not best-effort layouts.

## Delivery slices and acceptance criteria

### 1. Extension and secret boundaries

- VSIX extraction rejects absolute, traversal, drive/device and symlink entries.
- Download, file count, per-file, expanded-size and compression-ratio limits are
  enforced before activation; installation uses staging plus atomic replacement.
- Extension uninstall cannot remove a path outside the extension root.
- Auth and provider secrets use the operating-system protected secret store.
- Frontend and Node extension settings receive redacted metadata, never tokens.
- Extension Webviews default to extension/workspace resource roots and require a
  view-scoped message token.

### 2. Chat run correctness

- Starting a run is atomic and returns accepted/busy plus a unique run ID.
- Every callback, tool invocation and save is scoped to its immutable run context.
- A cancelled/stale run cannot write to a new conversation after its join timeout.
- Read caches are invalidated by writes, workspace changes and new conversations.
- Context compaction preserves whole user/assistant/tool interaction groups.

### 3. MCP safety and protocol support

- Unknown tools default to confirmation; read-only access uses an explicit
  per-server/tool policy, never a name heuristic or untrusted annotation alone.
- Workspace MCP processes require trust and receive an allowlisted environment.
- Streamable HTTP supports initialization, negotiated version/session headers,
  JSON or SSE responses, unique request IDs, pagination and clean shutdown while
  retaining a bounded legacy SSE fallback.

### 4. Frontend simplification

- 600x400 and 800px layouts remain usable without horizontal clipping.
- Settings navigation has a compact alternative rather than disappearing.
- Repeated provider/model/status controls are consolidated around the composer.
- Missing credentials lead directly to provider setup and connection testing.
- Muted text meets normal-text contrast requirements and frequent targets are at
  least 24 CSS pixels or have equivalent spacing.
- Settings shows its shell immediately and hydrates hidden sections lazily.

### 5. Truthful release gate

- Source-string assertions are updated to current intentional UX contracts and
  critical interaction checks run against a real DOM.
- Browser smoke scripts fail when Playwright is unavailable; `SKIP` is not green.
- The complete AI Editor self-test passes twice consecutively, frontend health is
  green, and both browser smoke suites explicitly report `PASS`.

## Rollback and compatibility

Public JS method names and persisted non-secret settings remain compatible.
Legacy plaintext secrets are migrated once and removed only after protected
storage succeeds. Legacy MCP SSE remains available as a fallback during the
Streamable HTTP transition. Each slice is committed separately.
