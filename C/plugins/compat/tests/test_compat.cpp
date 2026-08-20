// test_compat.cpp — smoke test
#include "sao/plugins/compat/py_v1_manifest.h"
#include "sao/plugins/compat/migration.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
[[noreturn]] void sao_test_assert_fail(const char* file, int line,
                                        const char* expression) {
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

#define SAO_TEST_ASSERT(...)                                                   \
    do {                                                                       \
        if (!(__VA_ARGS__)) {                                                  \
            sao_test_assert_fail(__FILE__, __LINE__, #__VA_ARGS__);            \
        }                                                                      \
    } while (false)
} // namespace
int main() {
    using namespace sao::plugins::compat;
    using namespace sao::plugins::loader;

    // guess_entry 真实装, 每种 language 猜对
    char* entry = nullptr;
    int32_t rc = sao_plugins_compat_guess_entry(engine_kind::python, &entry);
    SAO_TEST_ASSERT(rc == SAO_OK);
    SAO_TEST_ASSERT(entry != nullptr);
    SAO_TEST_ASSERT(std::strcmp(entry, "plugin.py") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::emma, &entry);
    SAO_TEST_ASSERT(rc == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(entry, "plugin.emma") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::angelscript, &entry);
    SAO_TEST_ASSERT(rc == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(entry, "plugin.as") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::lua, &entry);
    SAO_TEST_ASSERT(rc == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(entry, "plugin.lua") == 0);
    std::free(entry);

    entry = nullptr;
    rc = sao_plugins_compat_guess_entry(engine_kind::csharp, &entry);
    SAO_TEST_ASSERT(rc == SAO_OK);
    SAO_TEST_ASSERT(std::strcmp(entry, "plugin.cs") == 0);
    std::free(entry);

    // deprecated_entries 有内容
    size_t count = 0;
    const deprecated_entry* entries = sao_plugins_compat_deprecated_entries(&count);
    SAO_TEST_ASSERT(entries != nullptr);
    SAO_TEST_ASSERT(count >= 6);
    for (size_t i = 0; i < count; ++i) {
        SAO_TEST_ASSERT(entries[i].old_name != nullptr);
        SAO_TEST_ASSERT(entries[i].new_name != nullptr);
        SAO_TEST_ASSERT(entries[i].reason != nullptr);
        SAO_TEST_ASSERT(entries[i].category != nullptr);
    }

    std::printf("compat smoke test passed\n");
    return 0;
}
