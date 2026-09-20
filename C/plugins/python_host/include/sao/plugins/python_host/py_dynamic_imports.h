#pragma once

// py_dynamic_imports.h — runtime __imp_* resolution for the embedded host.
//
// Kept free of Python.h so py_import_stubs.cpp can define raw __imp_<name>
// globals without colliding with __declspec(dllimport) declarations.

namespace sao::plugins::python_host::detail {

// Fills every __imp_* slot from an already-loaded CPython runtime module.
// Returns false and reports the first unresolved export name when the
// module lacks a required symbol (version/feature mismatch → fail closed).
bool resolve_python_imports(void* module, const char** missing_name) noexcept;

// True once every slot has been filled; guards pre-resolution call sites
// (e.g. gil_scope_enter before the first pyhost init) from zero-slot calls.
bool python_imports_resolved() noexcept;

} // namespace sao::plugins::python_host::detail
