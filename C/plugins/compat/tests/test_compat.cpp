// test_compat.cpp — smoke test
#include "sao/plugins/compat/py_v1_manifest.h"
#include "sao/plugins/compat/migration.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

int main() {
    using namespace sao::plugins::compat;
    using namespace sao::plugins::loader;

    // guess_entry 真实装, 每种 language 猜对
    char* entry = nullptr;
    int32_t rc = sao_plugins_compat_guess_entry(engine_kind::python, &entry);
    assert(rc == SAO_OK);
    assert(entry != nullptr);
    assert(std::strcmp(entry, "plugin.py") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::emma, &entry);
    assert(rc == SAO_OK);
    assert(std::strcmp(entry, "plugin.emma") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::angelscript, &entry);
    assert(rc == SAO_OK);
    assert(std::strcmp(entry, "plugin.as") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::lua, &entry);
    assert(rc == SAO_OK);
    assert(std::strcmp(entry, "plugin.lua") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::csharp, &entry);
    assert(rc == SAO_OK);
    assert(std::strcmp(entry, "plugin.cs") == 0);
    std::free(entry);

    // deprecated_entries 有内容
    size_t count = 0;
    const deprecated_entry* entries = sao_plugins_compat_deprecated_entries(&count);
    assert(entries != nullptr);
    assert(count >= 6);
    for (size_t i = 0; i < count; ++i) {
        assert(entries[i].old_name != nullptr);
        assert(entries[i].new_name != nullptr);
        assert(entries[i].reason != nullptr);
        assert(entries[i].category != nullptr);
    }

    std::printf("compat smoke test passed\n");
    return 0;
}
