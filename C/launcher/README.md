# launcher — public bootstrap and native payload

The single end-user-facing entrypoint is `SaoAuto.exe`, a static-CRT bootstrap
with system-only imports. The original launcher remains the logical CMake target
`SaoAuto`, but its physical payload leaf is `f577a5067a96c920.exe` inside the
authenticated encrypted runtime bundle. Both processes are native and contain no
CPython runtime.

## Public bootstrap lifecycle

1. Resolve the install root from the bootstrap module path and open
  `runtime/ff22701a59858ebf` by an exact, non-reparse path identity.
2. Authenticate the encrypted manifest and entries with the generated bundle key.
3. Create `%LocalAppData%/SaoAuto/runtime` with a restrictive DACL and an owned
  `gen-<pid>-<nonce>` directory; extract the opaque payload and 16 opaque session DLLs there.
4. Atomically publish four opaque first-party dependencies beside the fixed
  `runtime/helper/WdiSvcHost.exe`; retain extraction handles for payload lifetime.
5. Set `SAO_INSTALL_ROOT`, `SAO_RUNTIME_STAGE`, `SAO_BOOTSTRAP_PID`, and a controlled
  `PATH`; create the payload suspended, assign a kill-on-close/breakaway job, and resume.
6. Wait for the payload, propagate its exit code, and remove the exact helper files
  and generation on normal and failure paths.

The bootstrap removes the hidden helper transaction lock when releasing the
transaction. Final waited probes found no opaque helper DLLs, transaction locks,
or LocalAppData generations in any Debug, RelWithDebInfo, or Hardened ship tree.

The bundle has 17 unique PE inputs and 21 destination records: one payload,
16 session DLL records, and four helper records that reuse four DLL inputs. The
bootstrap uses `CreateProcessW` and the normal Windows loader; it does not manual-map
the payload or any DLL.

The updater may explicitly break away from the job, waits for the outer bootstrap
PID, and restarts the installed `SaoAuto.exe`. The bundle handle is closed before
payload launch so the update transaction can replace it.

## Developer Preview

`sao_ui_preview` is a developer-only production-compositor shell. It participates
in the default build so first-party output-name or dependency changes relink it,
but it has no install rule and is absent from every ship tree. The current Debug
executable imports `4f1988ce57d0a1c0.dll`, `4d49c8a403c085f1.dll`,
`7bddbf75a3963c48.dll`, `5813f2d52b330ada.dll`, and `ed0fa2d377a29333.dll`;
a real launch reached input-idle with a nonzero top-level window handle titled
`SAO Classic · production UI shell`.

## Responsibilities

The payload launcher is deliberately **thin**. Every heavy feature lives in the
subsystem DLLs (`platform`, `security`, `shell`, `license`, `plugins`).  The
launcher's only job is to sequence subsystem initialisation, handle process
lifetime, and turn crashes into diagnosable minidumps.

Init pipeline order:

```
+----------------------------+
| 1. crash_handler           |   register SEH filter, minidump writer
+----------------------------+
| 2. single_instance guard   |   mutex on "Global\SaoAuto.Instance"
+----------------------------+
| 3. working_dir resolve     |   validated SAO_INSTALL_ROOT or legacy direct-launch fallback
+----------------------------+
| 4. license verify          |   phone home, decrypt shell key
+----------------------------+
| 5. shell integrity check   |   verify hardened build wasn't tampered
+----------------------------+
| 6. platform init           |   core, net, engine, ui, sdk, scripting
+----------------------------+
| 7. plugin discovery        |   scan plugins/, activate autostart
+----------------------------+
| 8. UI online               |   overlay compositor, panels, hotkeys
+----------------------------+
| 9. run message loop        |   main.cpp -> App::run() -> blocked here
+----------------------------+
| 10. shutdown sequence      |   reverse of init order, save state
+----------------------------+
```

Any payload failure at steps 1-8 aborts the launcher with a diagnostic dialog and
non-zero exit code (`SaoLauncherExitCode` enum in `app.h`).

## Dynamic Entity roots

The launcher deep-copies loader provider/root catalog views and projects them
through the existing owner-scoped D1 registry into one Entity UI ABI 1.5
complete-tree transaction. Root action references resolve only through the
launcher-owned `(provider_id, action_id)` route store; referenced rows are
removed from the built-in Plugins root to avoid duplicate actions.

Publication preserves `prepare -> UI -> commit/resync`: malformed or missing
root routes fail before the UI sink, a UI failure does not consume route
tokens, and a commit race resynchronizes the winner snapshot. Script hosts do
not receive route tokens and never call Entity UI directly.

## File layout

```
launcher/
+-- CMakeLists.txt
+-- README.md               (this file)
+-- manifest.xml            Win32 application manifest (DPI-aware, asInvoker)
+-- include/sao/launcher/
|   +-- app.h               main App class + exit codes
|   +-- args.h              command-line arg parsing
|   +-- crash_handler.h     SEH filter + minidump
|   +-- single_instance.h   named-mutex guard
|   +-- working_dir.h       BASE_DIR resolution
|   +-- init_pipeline.h     step-by-step subsystem bring-up
|   +-- shutdown.h          reverse-order teardown
+-- src/
|   +-- bootstrap_main.cpp  public bundle verifier/extractor/process owner
|   +-- main.cpp            wWinMain entry point
|   +-- app.cpp
|   +-- args.cpp
|   +-- crash_handler.cpp
|   +-- single_instance.cpp
|   +-- working_dir.cpp
|   +-- init_pipeline.cpp
|   +-- shutdown.cpp
+-- resources/
|   +-- app.rc              version resource + manifest include
|   +-- icon.ico            placeholder (or link to sao_auto/python/icon.ico)
|   +-- version.h           VERSIONINFO fields; ASCII FileDescription avoids mojibake
```

## Command line

```
SaoAuto.exe [options]       # bootstrap forwards the exact argument vector to the payload

Options (all optional):
  --safe-mode           Skip plugin discovery, load only core UI.
  --no-license          Bypass license verification (dev builds only, refuses to run in hardened builds).
  --config=<path>       Override config file location.
  --log-level=<lvl>     trace|debug|info|warn|error|critical
  --version             Print version and exit.
  --help                Print this help and exit.
```

## Exit codes

Payload exit codes are defined in `include/sao/launcher/app.h`; the bootstrap
propagates them unchanged after a successful launch. Bundle/path/extraction/process
startup failures return a non-zero bootstrap code before the payload runs.

| Code | Symbol                          | Meaning                                          |
|------|---------------------------------|--------------------------------------------------|
| 0    | `SAO_EXIT_OK`                   | Clean shutdown                                   |
| 1    | `SAO_EXIT_ALREADY_RUNNING`      | Another instance already running                 |
| 2    | `SAO_EXIT_LICENSE_INVALID`      | License check failed                             |
| 3    | `SAO_EXIT_SHELL_TAMPERED`       | Shell integrity check failed                     |
| 4    | `SAO_EXIT_PLATFORM_INIT_FAIL`   | Platform subsystem failed to initialise          |
| 5    | `SAO_EXIT_PLUGIN_LOAD_FAIL`     | Fatal plugin load error                          |
| 6    | `SAO_EXIT_UI_ONLINE_FAIL`       | UI could not come online                         |
| 7    | `SAO_EXIT_CRASH`                | Unhandled SEH — minidump was written             |
| 8    | `SAO_EXIT_BAD_ARGS`             | Invalid command line                             |

## Cross-subsystem contract

The launcher only calls into the C ABI exposed by
`include/sao/launcher/init_pipeline.h`.  That header uses opaque handles and
never leaks C++ types across subsystem boundaries. Logical target identities,
export names, C ABI, and structure layouts are unchanged by the opaque physical
basename map. A changed subsystem or physical map is relinked into the payload and
repacked into the bundle; the public bootstrap remains free of first-party imports.

Expected symbols each subsystem exports (all under `extern "C"`):

- Platform:  `sao_platform_bringup(sao_platform_config*, sao_platform_ctx**)`,
             `sao_platform_teardown(sao_platform_ctx*)`.
- Security:  `sao_security_init(sao_security_config*)`,
             `sao_security_shutdown(void)`.
- Shell:     `sao_shell_verify_integrity(sao_shell_verify_result*)`.
- License:   `sao_license_verify(sao_license_result*)`.
- Plugins:   `sao_plugins_discover(sao_platform_ctx*, sao_plugins_registry**)`,
             `sao_plugins_activate_autostart(sao_plugins_registry*)`,
             `sao_plugins_shutdown(sao_plugins_registry*)`.

If any of these symbols are missing at link time (because the subsystem
toggle was OFF), the launcher's CMakeLists skips linking that subsystem's
meta-target and the corresponding pipeline step is compiled out.

## Packaging and accepted evidence

`SAO_ENCRYPTED_RUNTIME_BUNDLE=ON` is the default. The relevant targets are
`SaoAuto` (logical payload), `SaoBootstrap` (physical `SaoAuto.exe`),
`sao_runtime_pack`, `sao_runtime_bundle`, `sao_runtime_bundle_audit`, and
`sao_release_acceptance`.

The complete Debug build passes. RelWithDebInfo and Hardened fresh acceptance each
report 22 direct shipped PEs, 76 exact manifest files, diagnostics absent, and 17
authenticated unique bundle PE inputs. Debug ship was refreshed to the same
22-PE/76-file direct surface. All three current trees contain zero plaintext
first-party DLLs and no manifest extras.
Debug, RelWithDebInfo, and Hardened `SaoAuto.exe --version` probes pass verify,
decrypt, normal Windows payload launch, exit-code propagation, and cleanup; each
leaves zero generation directories, transient first-party helper DLLs, and lock
files. VERSIONINFO reports `SAO Auto - native C++ launcher`, product `SAO Auto`,
original filename `SaoAuto.exe`, and version `0.2.0.0` without mojibake.

| Configuration | Bootstrap | Runtime bundle |
| --- | --- | --- |
| Debug | 2,445,312 bytes; SHA-256 `414cae5cdeaee548d902a1f117a10ef2887e6988e6a1130a34a870dc8a9534d4` | 74,069,584 bytes; SHA-256 `e01cf9efc7cce3354bb02aa99c8125fb06c88552bbe215a08fba6c30be88dd08` |
| RelWithDebInfo | 319,488 bytes; SHA-256 `1a2123d1bc36a1e4d4b61422e98941666df4cce2c4c311bf1995584ed6732540` | 26,507,856 bytes; SHA-256 `89e1181954367d5b399addb7db0e000369e6f03ebfd6be3997c025834fcd8dac` |
| Hardened | 322,048 bytes; SHA-256 `d93190d92f9075826f676cba967cdd8203998c1017e54f6916b2b7a0d5f571f4` | 26,741,328 bytes; SHA-256 `ac7654d89a4712a24c0879c85ea9b163f86a5d1c818cbcd2081d693c28a5aea9` |

All three current build directories contain the complete 17/17 opaque PE map,
and read-only bundle verification plus bootstrap key-layout audit succeeds for
each configuration. Their exported versions are UI ABI 1.16 (`65552`) and AI
Editor ABI 1.2 (`65538`); current build helper sidecars are RTIO protocol 3 / ABI
`65538`. The refreshed `windows-debug/ship` directory matches its current build
bootstrap and bundle hashes and has the same 22-PE/76-file opaque package surface.
