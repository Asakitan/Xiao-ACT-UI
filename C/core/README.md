# core/ — Phase 1 skeleton

Builds `sao_core.dll` (CMake target `sao_core`, alias `sao::core`). Phase-0 scaffolding only —
see `../docs/module-inventory.md`'s `## core/ — Phase 1` section for the research this is based
on, and `../docs/abi-contract.md` for the C ABI every export follows.

## Real

- `sao_core_abi_version()` / `sao_core_set_log_callback()` (`sao_core.h`/`sao_core.cpp`).
- `sao_core_process_open()` (`process.h`/`.cpp`) — `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
  ...)`, wraps the resulting `HANDLE` in a heap-allocated `sao_core_process_s`.
- `sao_core_process_close()` (`process.h`/`.cpp`) — `CloseHandle` + free, safe no-op on null.

## Stubbed (`SAO_ERR_NOT_IMPLEMENTED`)

- `sao_core_read_bytes()` (`process.h`/`.cpp`) — the batched, bounds-checked
  `NtReadVirtualMemory` path is later, separate work.
- `sao_core_class_index_resolve()` / `sao_core_class_index_resolve_field_offset()`
  (`class_index.h`/`.cpp`) — future live-klass/field-table resolution.
- `sao_core_scan_find_pattern()` / `sao_core_scan_find_aligned_u64()` (`scan.h`/`.cpp`) — future
  masked AOB scan and aligned-value-set scan over a caller-provided buffer.
- `sao_core_pixels_premultiply_blend()` / `sao_core_pixels_find_alpha_spans()`
  (`pixels.h`/`.cpp`) — future BGRA compositing math.
- `sao_core_window_create_layered_topmost()` (`window.h`/`.cpp`) — future layered/topmost overlay
  window creation.

## Linked but not yet used

`nlohmann-json` resolves and links (see `CMakeLists.txt`) but nothing here generates or consumes
JSON yet — future work persists the class-index RVA cache as JSON, per `module-inventory.md`.
