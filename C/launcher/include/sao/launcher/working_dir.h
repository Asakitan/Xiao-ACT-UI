// SAO Auto — launcher/working_dir.h
//
// Resolves the equivalent of Python's config.BASE_DIR — the root of the
// packaged app tree.  In the Python build this is `dirname(sys.executable)`
// (see project_nuitka_runtime_layout_code_pitfalls memory).  In the C build
// we follow the same convention:
//
//   base_dir = dirname(exe_path)
//
// unless the launcher is running from a `runtime/` subdirectory of an
// onedir bundle, in which case:
//
//   base_dir = dirname(dirname(exe_path))    // pop the runtime/ layer
//
// This matches build_nuitka.bat's onedir layout:
//
//   dist/release/XiaoACTUI/
//       SaoAuto.exe          (C launcher — this binary)
//       runtime/SaoAuto.exe  (Nuitka main EXE — irrelevant here but shape
//                             is identical to build_nuitka.bat)
//       plugins/, web/, ...

#pragma once

#include <windows.h>
#include <cstddef>
#include <string>

namespace sao::launcher {

// Resolve the process's exe path (via GetModuleFileNameW) and derive
// BASE_DIR from it.  Writes both back into AppState (exe_path, base_dir).
// Returns true on success.
struct AppState;
bool resolveWorkingDir(AppState& state) noexcept;
bool getCurrentModulePath(std::wstring& path_out) noexcept;

// Given exe_path, compute base_dir by the rules above.  Testable helper —
// takes explicit inputs, no globals.
bool computeBaseDir(const wchar_t* exe_path,
                    wchar_t* base_dir_out,
                    std::size_t base_dir_cap) noexcept;
bool computeBaseDir(const wchar_t* exe_path,
                    std::wstring& base_dir_out) noexcept;

// Ensure a directory exists under BASE_DIR (creates parents as needed).
// Used to prep `crash/`, `logs/`, `plugins/`, `data/` on first launch.
bool ensureDirectoryExists(const wchar_t* path) noexcept;

} // namespace sao::launcher
