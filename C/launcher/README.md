# launcher — SaoAuto.exe

The single end-user-facing binary produced by this repository.

## Responsibilities

The launcher is deliberately **thin**.  Every heavy feature lives in the
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
| 3. working_dir resolve     |   equivalent to Python config.BASE_DIR
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

Any failure at steps 1-8 aborts the launcher with a diagnostic dialog and
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
|   +-- version.h           VERSIONINFO fields (auto-generated from CMake)
+-- tests/
    +-- CMakeLists.txt
    +-- test_launcher_smoke.cpp
    +-- test_args.cpp
    +-- test_single_instance.cpp
    +-- test_working_dir.cpp
```

## Command line

```
SaoAuto.exe [options]

Options (all optional):
  --safe-mode           Skip plugin discovery, load only core UI.
  --no-license          Bypass license verification (dev builds only, refuses to run in hardened builds).
  --config=<path>       Override config file location.
  --log-level=<lvl>     trace|debug|info|warn|error|critical
  --version             Print version and exit.
  --help                Print this help and exit.
```

## Exit codes

Defined in `include/sao/launcher/app.h`:

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
never leaks C++ types across subsystem boundaries, so we can rebuild any one
of the subsystem DLLs without recompiling `SaoAuto.exe`.

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
