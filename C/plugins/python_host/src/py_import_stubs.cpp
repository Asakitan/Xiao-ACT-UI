// py_import_stubs.cpp — dynamic __imp_* storage for CPython embed symbols.
//
// The host deliberately does NOT link python3xx.lib: the launcher payload
// must start on machines that have no CPython installed, and the runtime is
// bound at use-time from the python311.dll discovered inside python_home
// (see load_python_runtime_module in py_host.cpp).  Python.h declares every
// PyAPI function/data __declspec(dllimport), so consuming objects emit calls
// and dereferences through __imp_<name> pointer slots.  This TU provides
// those slots and a resolver that fills them via GetProcAddress on the
// loaded runtime module — exactly what the OS loader would have done, but
// lazily and bound to the canonical python_home runtime, not to whatever
// python3.dll sits on PATH.
//
// The .lib pragma hints left in the objects are neutralized by the
// /NODEFAULTLIB link options sao_plugins_python_host propagates to every
// consumer; the __imp_* globals below are the entire binding surface.
//
// Slots stay nullptr until resolve_python_imports() succeeds; pyhost never
// issues a Py call before a runtime is loaded, so a zero slot is unreachable
// in practice (same contract as dynamic_python_data()).
//
// IMPORTANT: this TU must NOT include py_release_abi.h / Python.h — __imp_*
// names must stay plain untyped storage with no dllexport conflict.

#include "sao/plugins/python_host/py_dynamic_imports.h"

#if defined(SAO_HAS_PYTHON_EMBED) && defined(SAO_PYHOST_DYNAMIC_PY_DATA)

#include <windows.h>

extern "C" {
#define SAO_PY_IMPORT(name) void* __imp_##name = nullptr;
#include "py_required_exports.inc"
#undef SAO_PY_IMPORT
}

namespace sao::plugins::python_host::detail {

namespace {

struct python_import_slot {
    const char* name;
    void** slot;
};

const python_import_slot* python_import_table() noexcept {
    static const python_import_slot table[] = {
#define SAO_PY_IMPORT(name) { #name, reinterpret_cast<void**>(&__imp_##name) },
#include "py_required_exports.inc"
#undef SAO_PY_IMPORT
    };
    return table;
}

size_t python_import_count() noexcept {
    size_t n = 0;
#define SAO_PY_IMPORT(name) ++n;
#include "py_required_exports.inc"
#undef SAO_PY_IMPORT
    return n;
}

} // namespace

bool resolve_python_imports(void* module, const char** missing_name) noexcept {
    const python_import_slot* table = python_import_table();
    const size_t count = python_import_count();
    for (size_t i = 0; i < count; ++i) {
        const FARPROC resolved =
            GetProcAddress(static_cast<HMODULE>(module), table[i].name);
        if (resolved == nullptr) {
            if (missing_name != nullptr)
                *missing_name = table[i].name;
            return false;
        }
        *table[i].slot = reinterpret_cast<void*>(resolved);
    }
    if (missing_name != nullptr)
        *missing_name = nullptr;
    return true;
}

bool python_imports_resolved() noexcept {
    const python_import_slot* table = python_import_table();
    const size_t count = python_import_count();
    for (size_t i = 0; i < count; ++i) {
        if (*table[i].slot == nullptr)
            return false;
    }
    return true;
}

} // namespace sao::plugins::python_host::detail

#endif // SAO_HAS_PYTHON_EMBED && SAO_PYHOST_DYNAMIC_PY_DATA
