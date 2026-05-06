# SaoAuto C# Port

Parallel C# implementation of the Python/Cython `sao_auto` runtime.
Python remains canonical; the C# tree is built session-by-session and
reaches feature parity over the course of fourteen named sessions
(`docs/session-handoffs/session-NN.md`).

## Status

As of Session 14 — **256 unit tests passing, 0 failing**, all
foundational layers landed: configuration, state, CLI, packet protocol
+ TCP reassembler + parser envelope, vision contracts, overlay
scheduler / lanes / premultiply, menu state machine, WebView2 bridge
contract, panel snapshots + geometry + format, automation engines,
updater state machine + manifest, parity harness.

What still needs follow-up sessions (`*b` suffix):
- Live SharpPcap capture + per-method packet decode (5d).
- Live frame capture + recognition (6b).
- ULW window presenter + click-through (7b).
- Live entity menu rendering + hotkeys (8b).
- Live WebView2 host + HTML asset load (9b).
- Live overlay panel rendering (10b).
- Live SendInput / sound playback / Commander / HideSeek (11b).
- Live HTTP fetch + apply helper + onedir packaging (12b).
- Cross-runtime fixture generators (13b).
- Root `SAO-UI.sln` integration (14b).

See [docs/cutover-checklist.md](docs/cutover-checklist.md) for the full
gate list before the C# port can replace the Python runtime.

## Projects

| Project | Purpose |
| --- | --- |
| `SaoAuto.App` | WPF entry, mode router, entity host, WebView host, hotkeys, startup lifecycle |
| `SaoAuto.Core` | Configuration, GameState, packets, vision, automation engines, updater |
| `SaoAuto.Proto` | Generated Star Resonance protobuf types |
| `SaoAuto.Overlay` | Scheduler, lanes, BGRA frame buffer, panel snapshots, geometry |
| `SaoAuto.UpdateHost` | Local update manifest server (skeleton) |
| `SaoAuto.Tests` | Unit tests for every layer |
| `SaoAuto.ParityTests` | JSON parity harness vs Python expected fixtures |

## Documentation

- [docs/cutover-checklist.md](docs/cutover-checklist.md) — gates that
  must close before cutover.
- [docs/migration-notes.md](docs/migration-notes.md) — Python→C#
  compatibility notes for users / packagers.
- [docs/run-and-test.md](docs/run-and-test.md) — daily-loop recipes.
- [docs/parity-matrix.md](docs/parity-matrix.md) — feature parity status.
- [docs/source-map.md](docs/source-map.md) — Python module → C# project map.
- [docs/session-handoffs/](docs/session-handoffs/) — per-session
  scope, decisions, gaps, and "next session" pointer.

## Quick Verification

```powershell
dotnet restore .\SaoAuto.sln --configfile .\NuGet.config
dotnet build   .\SaoAuto.sln
dotnet test    .\SaoAuto.sln --no-build
```

## Guardrails

- Do not edit Python runtime files as part of normal C# port sessions.
- Preserve both `entity` and `webview` UI modes.
- Preserve the entity SAO popup menu and the WebView menu/bridge path.
- Do not run release packaging, dev publish, remote upload, or GitHub
  push unless the current session explicitly asks.
- Keep overlay work mindful of 60 Hz visible panels, top-down
  premultiplied BGRA, click-through state, and DXGI/WGC thread affinity.
