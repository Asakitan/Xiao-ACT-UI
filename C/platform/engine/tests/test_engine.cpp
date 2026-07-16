#include <catch2/catch_test_macros.hpp>

#include "sao/engine/abi.h"
#include "sao/engine/ui_spec.h"

TEST_CASE("engine ABI version is non-zero", "[engine][abi]") {
    REQUIRE(sao_engine_abi_version() == SAO_ENGINE_ABI_VERSION);
}

TEST_CASE("ui_spec version constant matches header", "[engine][ui_spec]") {
    REQUIRE(sao_engine_ui_spec_version() == SAO_UI_SPEC_VERSION);
}
