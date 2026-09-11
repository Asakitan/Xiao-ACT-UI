#pragma once

#include "sao/ui/sound.h"

#include <array>
#include <cstdint>
#include <memory>

namespace sao::ui::sound_detail {

enum class LinkStartPlaybackState {
    silent,
    playing,
    complete,
    interrupted,
};

struct LinkStartAudioSnapshot {
    uint32_t sample_rate{44100u};
    std::array<uint64_t, 3> cue_end_frames{44116u, 216106u, 463080u};
    uint64_t samples_played{};
    LinkStartPlaybackState state{LinkStartPlaybackState::silent};
};

struct LinkStartAudioPlayback;
using LinkStartAudioHandle = std::shared_ptr<LinkStartAudioPlayback>;

sao_status_t linkstart_audio_info(LinkStartAudioSnapshot* out_snapshot) noexcept;
sao_status_t linkstart_audio_begin(sao_ui_sound_group_t group, int32_t requested_volume,
                                   LinkStartAudioHandle* out_playback) noexcept;
sao_status_t linkstart_audio_snapshot(const LinkStartAudioHandle& playback,
                                      LinkStartAudioSnapshot* out_snapshot) noexcept;

} // namespace sao::ui::sound_detail
