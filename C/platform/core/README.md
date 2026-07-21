# `platform/core`

Universal runtime primitives — status codes, logging, config, crypto,
memory read, process/window enumeration, timing, strings, paths, threads,
events, error info.  Everything the other platform modules need before
any game/plugin logic runs.

## Corresponding Python source

| C++ header               | Python source                                 |
| ------------------------ | --------------------------------------------- |
| `status.h`, `error.h`    | `sao_auto/python/errors.py` (informal)        |
| `logging.h`              | `sao_auto/python/logging_setup.py`            |
| `config.h`               | `sao_auto/python/config.py`                   |
| `crypto.h`               | `sao_auto/python/*/*_encrypt_drivers.py`      |
| `memory.h`, `process.h`  | `sao_auto/python/mem_probe/process.py`, `rt_io.py` |
| `window.h`               | `sao_auto/python/gui_modules/window.py`       |
| `time.h`                 | `time.perf_counter` scattered usage           |
| `string.h`, `path.h`     | scattered helpers throughout the tree         |
| `thread.h`, `event.h`    | `threading.Thread`, `Event` scattered usage   |

## Implementation map

- **Implemented slice** — process open/enumeration/info, module enumeration,
  `NtReadVirtualMemory`, typed reads, signed pointer chains, readable-region
  enumeration and thread-local process/memory error context. The focused
  current-process probe passes; Python fixture and live-game parity remain open.
- **Next core slice** — real Win32 implementations for `window`, `time`,
  `logging`, wait events and remaining lifecycle helpers.
- **Configuration and services** — `config` JSON backend, `crypto` via `../security/crypto/`,
  `thread` pool, `path` BASE_DIR resolver.
- **Later parity gates** — Python process/memory fixtures, target lease behavior,
  live game reads and orderly teardown.

## Public headers

- `abi.h` — SAO_CORE_API export macro + `sao_core_abi_version()`.
- `status.h` — the canonical `sao_status_t` enum.
- `logging.h` — level-based logger with a single install-able callback.
- `config.h` — typed get/set over a JSON-backed store.
- `crypto.h` — SHA/HMAC/AES-GCM/random.
- `memory.h` — read-only remote-process memory.
- `process.h` — open/enumerate/find process + module enumeration.
- `window.h` — screen info + HWND lookup + geometry helpers.
- `time.h` — monotonic + wall clock + sleep + scope timer.
- `string.h` — UTF-8/UTF-16 conversion + FNV-1a hash.
- `path.h` — BASE_DIR + join + mkdir -p + canonicalize.
- `thread.h` — shared work-stealing pool + periodic timer.
- `event.h` — auto/manual reset event primitive.
- `error.h` — thread-local rich error record.

## ABI stability rule

`SAO_CORE_ABI_VERSION` (in `abi.h`) is the only thing plugins actually
check.  Bump the minor when a struct grows a field at the end.  Bump the
major when any struct changes layout, or when a function's signature
changes.
