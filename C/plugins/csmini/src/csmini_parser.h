// csmini_parser.h — source → AST entry point (mirrors pymini_parser.h role).
#pragma once

#include "csmini_ast.h"

namespace sao::plugins::csmini {

// Parse `source` (utf8); cs_error distinguishes SyntaxError from UnsupportedFeature.
// `flags` (optional) accumulates the feature census used by the composite
// adapter's preflight (which constructs were seen).
ast_program parse_source(std::string_view source, const std::string& file,
                         feature_flags* flags = nullptr);

// Nonexecuting parse/classification: false + reason only for unsupported features;
// syntax/lexer cs_error propagates, and success clears out_reason.
bool csmini_preflight_subset(std::string_view src, std::string* out_reason);

} // namespace sao::plugins::csmini
