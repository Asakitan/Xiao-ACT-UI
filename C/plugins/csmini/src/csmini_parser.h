// csmini_parser.h — source → AST entry point (mirrors pymini_parser.h role).
#pragma once

#include "csmini_ast.h"

namespace sao::plugins::csmini {

// Parse `source` (utf8) into a program AST.  Throws cs_error on SyntaxError.
// `flags` (optional) accumulates the feature census used by the composite
// adapter's preflight (which constructs were seen).
ast_program parse_source(std::string_view source, const std::string& file,
                         feature_flags* flags = nullptr);

// Cheap lexical subset check: false = source needs an out-of-subset feature
// (unsafe/pointers/ref-out-in params, generics beyond List<>/Dictionary<,>,
// LINQ/async/partial/extension methods/attributes, out-of-subset usings);
// `out_reason` names the feature.
bool csmini_preflight_subset(std::string_view src, std::string* out_reason);

} // namespace sao::plugins::csmini
