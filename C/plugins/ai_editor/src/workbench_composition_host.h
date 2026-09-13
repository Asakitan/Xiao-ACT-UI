#pragma once
#include <cstdint>
#include <string>

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/core/status.h"
#include "sao/rt_io/proxy.h"
#include "sao/ui/compositor.h"

namespace sao::ai_editor::workbench {

struct CompositionHost;
using PanelSnapshotFn = std::string (*)(void*);
using PanelActionFn = void (*)(const char*, const uint8_t*, size_t, void*);
void set_panel_bridge(CompositionHost* host, PanelSnapshotFn snapshot, PanelActionFn action,
                      void* user_data) noexcept;

sao_status_t create(sao_ui_compositor_handle_t compositor, sao_ai_editor_launcher_t launcher,
                    CompositionHost** out_host) noexcept;

sao_status_t show(CompositionHost* host) noexcept;
sao_status_t hide(CompositionHost* host) noexcept;
sao_status_t tick(CompositionHost* host) noexcept;
sao_status_t try_destroy(CompositionHost* host) noexcept;
sao_status_t bind_memory_target(CompositionHost* host, sao_rt_io_proxy_handle_t proxy,
                                std::uint32_t pid, std::uint64_t start_time_100ns,
                                std::uint64_t selection_generation) noexcept;
sao_status_t clear_memory_target(CompositionHost* host) noexcept;

bool available(const CompositionHost* host) noexcept;
bool ready(const CompositionHost* host) noexcept;
bool failed(const CompositionHost* host) noexcept;
bool consume_close_request(CompositionHost* host) noexcept;

} // namespace sao::ai_editor::workbench
