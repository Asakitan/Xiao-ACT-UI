#include "sao/plugins/angel_host/as_error.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_take_exception(asIScriptContext* ctx, char** out_utf8) {
    if (out_utf8 != nullptr)
        *out_utf8 = nullptr;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (ctx == nullptr || out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (ctx->GetState() != asEXECUTION_EXCEPTION)
        return SAO_OK;

    try {
        int column = 0;
        const char* section = nullptr;
        const int line = ctx->GetExceptionLineNumber(&column, &section);
        const char* message = ctx->GetExceptionString();
        const asIScriptFunction* function = ctx->GetExceptionFunction();
        const char* declaration =
            function == nullptr ? nullptr : function->GetDeclaration(true, true, true);

        std::ostringstream stream;
        stream << "AngelScript exception: " << (message == nullptr ? "unknown exception" : message)
               << "\nfunction: " << (declaration == nullptr ? "<unknown>" : declaration)
               << "\nlocation: "
               << (section == nullptr || section[0] == '\0' ? "<unknown>" : section) << ':' << line
               << ':' << column;
        const std::string text = stream.str();
        auto* output = static_cast<char*>(std::malloc(text.size() + 1));
        if (output == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        std::memcpy(output, text.c_str(), text.size() + 1);
        *out_utf8 = output;
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)ctx;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::angel_host
