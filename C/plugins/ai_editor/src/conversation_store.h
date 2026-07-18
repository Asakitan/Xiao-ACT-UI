#pragma once

#include <string>
#include <string_view>

#include "scope_store.h"

namespace sao::ai_editor::native {

class ConversationStore final {
public:
    explicit ConversationStore(const ScopeStore& scopes) noexcept;

    int32_t create(std::string_view title,
                   std::string_view model,
                   std::string_view scope,
                   Json& result) const;
    int32_t append(std::string_view conversation_id,
                   const Json& message,
                   Json& result) const;
    int32_t get(std::string_view conversation_id, Json& result) const;
    int32_t list(std::string_view scope,
                 uint32_t limit,
                 Json& result) const;
    int32_t search(std::string_view query,
                   std::string_view scope,
                   uint32_t limit,
                   Json& result) const;
    int32_t remove(std::string_view conversation_id, Json& result) const;

    int32_t export_all(std::string_view scope, Json& result) const;
    int32_t import_conversation(const Json& conversation,
                                std::string_view scope,
                                bool overwrite,
                                std::string& out_id) const;

private:
    int32_t locate(std::string_view conversation_id,
                   std::filesystem::path& path) const;
    int32_t save(const std::filesystem::path& path, const Json& value) const;

    const ScopeStore& scopes_;
};

}  // namespace sao::ai_editor::native
