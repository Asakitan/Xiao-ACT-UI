// pymini_parser.h — source → AST entry point.
#pragma once

#include "pymini_ast.h"

namespace sao::plugins::pymini {

// Parse `source` (utf8) into a module AST.  Throws py_error on SyntaxError /
// UnsupportedSyntax.  `flags` (optional) accumulates the feature census used
// by the composite adapter's preflight (which constructs were seen).
ast_module parse_source(std::string_view source, const std::string& file,
                        feature_flags* flags = nullptr);

} // namespace sao::plugins::pymini
