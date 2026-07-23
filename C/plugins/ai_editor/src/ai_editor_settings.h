#pragma once

#include <cstdint>
#include <string>

#include "native_secret_store.h"
#include "scope_store.h"

namespace sao::ai_editor::native {

class AiEditorSettings final {
public:
    static Json defaults();
    static Json describe(const ScopeStore& scopes);

    static int32_t load(const ScopeStore& scopes,
                        SecretStore& secrets,
                        const Json& params,
                        Json& result);

    static int32_t save(const ScopeStore& scopes,
                        SecretStore& secrets,
                        const Json& params,
                        Json& result);

    static int32_t prepare_runtime_request(const ScopeStore& scopes,
                                           const Json& request,
                                           Json& prepared_request);

    static int32_t prepare_chat_request(const ScopeStore& scopes,
                                        SecretStore& secrets,
                                        const Json& request,
                                        Json& prepared_request,
                                        Json& provider,
                                        std::string& api_key);
};

}  // namespace sao::ai_editor::native
