#pragma once

#include "entity_provider_publication_internal.h"
#include "sao/ui/plugin_tabs.h"

#include <string_view>

namespace sao::launcher::plugin_tabs_publication {

sao_status_t publish(sao_ui_plugin_tabs_handle_t tabs,
                     const entity_action_routes::EntityActionRouteStore& routes,
                     const entity_provider_publication::EntityProviderPublicationState& state,
                     bool loader_bound, std::string_view locale = {}) noexcept;

}
