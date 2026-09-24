#pragma once

// Internal seams of overlay_host shared with the input router /
// compositor RGN-sync path.  Not part of the exported C ABI.
//
// The public sao_ui_overlay_host_set_input_region keeps its documented
// transactional semantics: the applied region is `current U stored
// previous` so a shrinking or moving layer never drops hit-test
// coverage mid-swap.  Callers that already carry temporal-union
// coverage in their rect list (the compositor's sync_host_rgn does,
// see SaoCompositorConfig::enable_temporal_union) must instead route
// through set_input_region_ex with kInputRegionSkipPrevUnion so the
// two union layers do not stack and over-pad the region during motion.

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/overlay_host.h"

namespace sao::ui::overlay_host_detail {

// Flags accepted by set_input_region_ex.  0 keeps the documented
// current-U-previous behaviour.
//
// kInputRegionSkipPrevUnion: apply only the provided rect list; the
// stored previous-rect list is still updated but is not unioned into
// the applied region.  The compositor's RGN-sync path passes this
// because its own temporal-union gate already folds the previous
// frame's spans into the list, and the flag also keeps the
// union-disabled ("pixel-exact") path at exactly the caller-provided
// geometry.
inline constexpr uint32_t kInputRegionSkipPrevUnion = 1u << 0u;

void set_menu_cursor(sao_ui_overlay_host_handle_t host, bool visible) noexcept;
void set_input_cursor(sao_ui_overlay_host_handle_t host, int32_t cursor_kind) noexcept;

sao_status_t set_input_region_ex(sao_ui_overlay_host_handle_t handle,
                                 const SaoOverlayHostInputRect* rects,
                                 size_t rect_count, uint32_t flags) noexcept;

} // namespace sao::ui::overlay_host_detail
