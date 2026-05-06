# SaoAuto C# Packaging

Local packaging scripts for the C# port. The Python equivalent is
`sao_auto/build_release.bat` + `build_full_package.py` + `build_delta.py`;
those still drive the production Python build until cutover.

## Goals

- Produce an onedir layout matching the Python release: `XiaoACTUI.exe`,
  `update.exe`, `web/`, `assets/`, `proto/`, `runtime/`, `config/`, `fonts/`.
- Stamp the version from `SaoAuto.Core.Configuration.AppVersion` at publish time.
- **Never** push to remote endpoints. Local-only by default.

## Status

Skeleton only. Live publish (full + delta) lands in Session 12b alongside:

- Atomic file mover with rollback (mirrors Python `update_apply.py`).
- Runtime-delta vs full-package decision helper.
- `_swap_update_*.cmd` script generator for self-replace of `update.exe`.

## Manual Build (for now)

```powershell
dotnet publish .\src\SaoAuto.App\SaoAuto.App.csproj -c Release -r win-x64 --self-contained false
```

Outputs to `src/SaoAuto.App/bin/Release/net8.0-windows/win-x64/publish/`.

`update.exe` lives in the runtime folder once we package the helper —
Session 12b will produce both binaries side-by-side.
