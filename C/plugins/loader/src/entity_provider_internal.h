#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sao/plugins/loader/entity_provider.h"

namespace sao::plugins::loader {

struct entity_provider_state;
struct plugin_handle_s;

int32_t register_entity_provider(
    const std::shared_ptr<plugin_handle_s>& owner,
    const std::string& owner_plugin_id,
    const char* provider_id_utf8,
    entity_snapshot_callback_fn snapshot,
    entity_action_handler_fn action_handler,
    void* user_data,
    std::shared_ptr<entity_provider_state>& out) noexcept;

bool entity_provider_is_current_thread(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept;

int32_t activate_entity_providers(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept;

int32_t deactivate_entity_providers(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept;

int32_t destroy_entity_providers(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept;

} // namespace sao::plugins::loader
