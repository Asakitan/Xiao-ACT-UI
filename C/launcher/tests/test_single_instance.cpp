// SAO Auto — launcher/tests/test_single_instance.cpp

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/single_instance.h"

using namespace sao::launcher;

TEST_CASE("acquire twice reports collision", "[launcher][single_instance]") {
    HANDLE a = nullptr;
    HANDLE b = nullptr;

    REQUIRE(acquireSingleInstance(a));
    REQUIRE(a != nullptr);

    // Second acquire from the same process must fail — the mutex name is
    // derived from the exe path, which is the same for both.
    REQUIRE_FALSE(acquireSingleInstance(b));
    REQUIRE(b == nullptr);

    releaseSingleInstance(a);
}

TEST_CASE("release + reacquire succeeds", "[launcher][single_instance]") {
    HANDLE a = nullptr;
    REQUIRE(acquireSingleInstance(a));
    releaseSingleInstance(a);

    HANDLE b = nullptr;
    REQUIRE(acquireSingleInstance(b));
    releaseSingleInstance(b);
}
