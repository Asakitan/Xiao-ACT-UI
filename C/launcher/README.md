# launcher — public bootstrap and native payload

The single end-user-facing entrypoint is `SaoAuto.exe`, a static-CRT bootstrap
with system-only imports. The original launcher remains the logical CMake target
`SaoAuto`, but its physical payload leaf is `f577a5067a96c920.exe` inside the
authenticated encrypted runtime bundle. Both processes are native and contain no
CPython runtime.

## Startup diagnostics and native dispatch

Link Start's parked SYSTEM hold now resumes from its first post-release tick,
excluding synchronous UI bring-up from the exit clock. Only final-stage100% plus
successful release starts the default SYSTEM perspective camera flyby; all six
loading stages and the final capture-sweep wait remain unchanged. Natural completion
then opens the existing geometric menu entrance. Offline root `--intro-handoff`
checks each stage,100%-but-not-released, repeated release and a3s delayed first tick
without backend startup; `--frame-ms` is relative to the resumed frame.
Current verification is recorded in `../docs/session-log/session-78-intro-menu-handoff.md`.

Product startup, including Debug, selects the native lifecycle without loading
historical rollout/dual-run configuration. It does not hand off to the retired
Python application on startup failure. Compatibility APIs and the explicit
acceptance-only dispatch remain separate; optional Python plugins are unchanged.

The bundle target also stages its authenticated output into
`bin/<config>/runtime/ff22701a59858ebf`, beside the build bootstrap. `SaoBootstrap`
depends on that target and, when present, `SaoRtIoHelper`, whose existing build
steps stage its executable, identity sidecar and runtime dependencies. Neither
bundle inputs nor helper dependencies lead back to the bootstrap in the static
source graph. Install still consumes the canonical `runtime-bundle` output.
A bare bootstrap without this file exits 10 before payload launch.

`--log-level=trace` reports launcher failure stages. Startup/exit fault evidence
captures duplicated raw stdout handles before subsystem bring-up so later
GUI-subsystem handle changes cannot hide it: intro-pump and background-worker results,
anti-debug non-clean evidence, `SAO_PROCESS_RETURN`, and outer-bootstrap
`SAO_BOOTSTRAP_CHILD_EXIT`/`CLEANUP`/`OUTCOME` checkpoints. These lines appear
only when stdout is inherited or a fault path is exercised; clean GUI startup
does not gain a new console dependency.

Session-63's `-20/stage16/reason26/error31` and Debug
`-4080/stage8/reason22/error6667` observations are repair inputs, not current
ordinary-start results. The strict-handle request still requires both permanent
bits, and no host requirement was weakened. Session-64's final accepted
RelWithDebInfo public bootstrap reaches all six Link Start stages and
`BOOTSTRAP_READY`; a targeted close of the payload's `4F5A.mh` owner window
returns payload/outer code zero with `guards=1 tree=0` and zero helper/process/
service/key/generation residue. That lifecycle used a temporary no-license
fixture which was deleted; the license gate is currently unconditional
fail-closed — with `license.enabled` a failed verification (no token,
expired, revoked, HWID mismatch) exits with code 2. (The `license.required`
advisory-degrade field documented for session-66 does not exist in the
current `LicenseProviderConfiguration`, `parseLicense`, shipped config, or
either gate; restore it in code before relying on degrade-to-free. See
`../docs/session-log/session-73-license-cli-selfheal-kit.md`.)
See
`../docs/session-log/session-64-overlay-and-rtio-helper-lifecycle.md`.

## Public bootstrap lifecycle

Normal GUI exit first hides registered panels, the Entity/NerveGear surface and
other native layers, without transferring their teardown ownership. The existing
owner-thread exit pump then runs 900ms Link End followed by 650ms CRT line/dot
shutdown before hiding the host and proceeding to resource teardown. Its cooperative
deadline is 2500ms; reduced motion remains 180ms. System shutdown, error, smoke,
safe and operator paths retain their existing skip/cancel behavior.

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
The final Session-64 public probe additionally emitted child exit zero,
`SAO_BOOTSTRAP_CLEANUP guards=1 tree=0`, and `failed=0 code=0`; direct Services
registry census found no `WdiSvcHost_*` key after exit.

The bundle has 17 unique PE inputs and 21 destination records: one payload,
16 session DLL records, and four helper records that reuse four DLL inputs. The
bootstrap uses `CreateProcessW` and the normal Windows loader; it does not manual-map
the payload or any DLL.

The updater may explicitly break away from the job, waits for the outer bootstrap
PID, and restarts the installed `SaoAuto.exe`. The bundle handle is closed before
payload launch so the update transaction can replace it.

## Native auto-update lifecycle

Update downloads are owned under `%LocalAppData%/SaoAuto/update-staging/run-*`.
The launcher binds each run to an exact owner marker, downloaded archive,
manifest version/SHA-256, installed outer entry, helper process, and outer
bootstrap parent identity; unknown leaves, reparse paths, malformed markers, or
identity drift fail closed.

Launcher and helper share `sao_update_handoff_v1_t`, a fixed 192-byte ABI-v1
record containing phase/status, parent/helper/restart PIDs, version, SHA-256, and
zeroed reserved bytes. `helper.ready` publishes `READY/PENDING` before ownership
transfer. After the installed entry passes its bounded `--version` probe, the
helper creates the real outer bootstrap suspended; `helper.complete` publishes
`RESTARTING/PENDING` before resume and terminal `COMPLETE/status` after commit,
transaction cleanup, and marker removal.
Every read revalidates exact file/struct size, ABI, legal phase, nonzero
parent/helper PIDs, zero flags/reserved, and version/SHA shape. READY additionally
matches the expected parent/helper PIDs and zero restart PID. Modern completion
instead matches the restarted outer-bootstrap PID and current version/SHA,
allows only `RESTARTING/PENDING` before `COMPLETE`, then requires status zero and
helper exit; it does not exact-match completion parent/helper PIDs to old READY.

The lifecycle is bounded: READY admission is five minutes, launcher-owned helper
termination is two seconds, restarted-launcher completion acknowledgement is thirty
seconds, helper parent exit is two minutes, and the installed-entry `--version` health
probe is thirty seconds. A failed probe rolls back replaced leaves,
removes transaction-created leaves, preserves files absent from the package, and
publishes the terminal failure status. A successful transaction commits the
overlay and leaves the restarted product to acknowledge completion.

`App::run()` calls `acknowledgeCompletedAutoUpdate()` after working-directory and
provider configuration but before license, security, platform, or plugin bring-up
can fail. Modern completion must match the restarted outer-bootstrap PID and
current version/SHA; helpers that predate `helper.complete` are reconciled only
when the exact owner marker is valid, the old owner is gone, READY/preservation
markers are absent, the archive SHA/current version match, and all leaves are known.
The production `0.2.1` migration
from a genuine `0.2.0` launcher/helper completed with exit 0, matched the current
ship bootstrap hash, and left no staging, transaction, helper, or fixture process
residue.

## Developer Preview

The process page now samples ordinary OS CPU, working-set, thread and architecture
metadata on its worker every three seconds, alongside system memory, process count
and uptime. CPU/memory charts retain up to 60 samples; unavailable readings remain
explicit, and each page contains twelve process rows. Preview shutdown leaves the
outer message loop before cancelling refresh and draining owners; late input is
suppressed and quit codes are preserved. Debug and RelWithDebInfo builds include
this change; the six-case Debug close probe and populated desktop charts passed.
Ship trees and Hardened were not refreshed for this slice.

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

The loader catalog is consumed only while overall plugin runtime authority is `ready`.
For overall non-ready/degraded runtime state, publication substitutes an empty catalog,
closes stale invocation state, clears the previous revision/content token and
published catalog, and republishes the built-in fail-closed surface. Built-in
roots remain exactly `Control`, `Tools`, `Plugins`, `Skins`, and `About`; the
`Control` root independently owns exactly ten child rows. Python runtime status
(`READY`, `UNCONFIGURED`, `UNAVAILABLE`, or `HOST_UNAVAILABLE`) is projected
separately; Python degradation alone does not set overall plugin degradation.

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
| 2    | `SAO_EXIT_LICENSE_INVALID`      | License check failed while `license.required` is true |
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

Session-63 restored the local build inputs and refreshed all three ship trees.
Debug/RelWithDebInfo full builds and both optimized release acceptances passed;
Hardened was built through the acceptance target's product dependency closure,
not a separate full `ALL_BUILD`. Debug ship staging also passed. Each ship has
22 direct PEs / 76 files, and all six build/ship version probes returned
`SaoAuto 0.2.1` with exit 0. Current bundle sizes and SHA-256 are recorded in
`../docs/session-log/session-63-startup-diagnostics.md`; they agree across each
preset's canonical, adjacent and ship copies. Session-64 subsequently rebuilt
the RelWithDebInfo bootstrap/bundle and passed final acceptance again: 17 input
PEs, 31,024,208 authenticated bundle bytes, 22 shipped PEs, and 76 exact files.
That accepted public ship reaches six-stage READY and normal exit zero under the
temporary lifecycle fixture; a failed license verification now degrades to the
free tier under the shipped `"required": false` rather than exiting 2.
Optimized C4702 warnings remain.

The earlier session-58b RelWithDebInfo release acceptance produced the
22-PE/76-file direct surface used for the production `0.2.1` package. That package
is 62,162,440 bytes with SHA-256
`1dab8e44cee5a6c3738d7d4da45c15b5566ffbf1ce8c812f6a6be61fa9c887f1`;
its public bootstrap, verified after the live migration, has SHA-256
`cf762fff390249698d18ed504b57fa6794bc7758a922885905689eb79df25670`.
The table below preserves earlier configuration snapshots, not the refreshed
session-63 bundle identities or the production archive identity. Local rebuilds
are not evidence of a newly published archive.
The release gate runs `sao_runtime_pack --verify-inputs` to match the current
17 plaintext PEs against all 21 authenticated leaf/destination/payload/size/hash
records. Bootstrap keeps exact extracted-file guards, rechecks every published
file against the authenticated manifest, and owns helper cleanup before lock release.
The v4.17 `SaoAuto.exe --version` probes established verify, decrypt, normal
Windows payload launch, exit-code propagation, and cleanup. Session-58b adds a
real old-client production update followed by `SaoAuto 0.2.1` exit 0 and an empty
update-staging/process census. At the session-34 cutoff, Debug had not rerun
key-layout or `--version` after its asset refresh; session-63 version results
above apply to the newly rebuilt bundle, not that historical file.

| Historical configuration snapshot | Bootstrap | Runtime bundle |
| --- | --- | --- |
| Debug | 2,445,312 bytes; SHA-256 `c3cc5bfb4f2d13658833aeb3066e784caa9963ab4b8dc3299b402d0e4b5fe455` | 74,635,856 bytes; SHA-256 `4046f800eff3c3650f5bdc090c9a36980428b6e8b9557f5f922318bbea410eb1` |
| RelWithDebInfo | 323,072 bytes; SHA-256 `165aad5f2fdb44f2c016bbe1461f13cb5baaeba355467a62f37e5efdd4e6b48a` | 26,625,616 bytes; SHA-256 `6ffa6a8493850d10a7d4e933a9fe5c7d7eaaf37d0634c9c6608dacec421df4d3` |
| Hardened | 326,656 bytes; SHA-256 `24984ded8ed990ef6aca619c9a7a95880187059402481696ba848518491af376` | 26,861,648 bytes; SHA-256 `32d3cb510387bbf51764723826680734431215850fcc979a6dcafa3a5306b82c` |

The session-34 Debug ship matched that historical build's bootstrap/bundle hashes
and passed its five-driver bundle audit. Session-63 has since refreshed Debug and
completed fresh Release/Hardened acceptance; the former pending-Hardened claim
is historical. Packaging results alone do not establish startup or driver
retirement; Session-64 separately proves the temporary-fixture public lifecycle,
while the empty process census proves process absence only.
