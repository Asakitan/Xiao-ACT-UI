// SAO Auto - shared fisheye glass backdrop service.

#include "sao/ui/fisheye_backdrop.h"

#include "sao/ui/dxgi_dup.h"
#include "sao/ui/theme.h"
#include "sao/ui/animator.h"
#include "fisheye_backdrop_gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kBytesPerPixel = 4;
constexpr size_t kMaxFrameBytes = static_cast<size_t>(SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES);
constexpr uint32_t kLiveAcquireTimeoutMs = 16;
constexpr auto kLiveRetryDelay = std::chrono::milliseconds(120);
constexpr auto kLiveCreateRetryDelay = std::chrono::milliseconds(300);

constexpr auto kGlassBackground =
    sao::ui::kSaoThemeGlassColors[SAO_UI_TOKEN_APP_BG];
constexpr auto kGlassBorder =
    sao::ui::kSaoThemeGlassColors[SAO_UI_TOKEN_APP_BORDER];
constexpr auto kGlassCyan =
    sao::ui::kSaoThemeGlassColors[SAO_UI_TOKEN_CORNER_CYAN];
constexpr auto kGlassGold =
    sao::ui::kSaoThemeGlassColors[SAO_UI_TOKEN_CORNER_GOLD];

std::atomic<uint64_t> g_backdrop_name_sequence{0};

struct BackdropGeometry {
    SaoUiFisheyeBackdropRect rect{};
    int32_t z_order{};
};

bool same_rect(const SaoUiFisheyeBackdropRect& lhs, const SaoUiFisheyeBackdropRect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_geometry(const BackdropGeometry& lhs, const BackdropGeometry& rhs) {
    return same_rect(lhs.rect, rhs.rect) && lhs.z_order == rhs.z_order;
}

bool checked_frame_layout(uint32_t width, uint32_t height, uint32_t stride, size_t* out_bytes) {
    if (out_bytes == nullptr || width == 0 || height == 0 ||
        width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    const uint64_t tight_stride = static_cast<uint64_t>(width) * kBytesPerPixel;
    if (tight_stride > std::numeric_limits<uint32_t>::max() || stride < tight_stride ||
        static_cast<uint64_t>(height) >
            std::numeric_limits<size_t>::max() / static_cast<uint64_t>(stride)) {
        return false;
    }
    const size_t bytes = static_cast<size_t>(stride) * height;
    if (bytes > kMaxFrameBytes) {
        return false;
    }
    *out_bytes = bytes;
    return true;
}

bool valid_rect(const SaoUiFisheyeBackdropRect& rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        return false;
    }
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
    if (right < std::numeric_limits<int32_t>::min() ||
        right > std::numeric_limits<int32_t>::max() ||
        bottom < std::numeric_limits<int32_t>::min() ||
        bottom > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    size_t ignored = 0;
    const uint64_t stride64 = static_cast<uint64_t>(rect.width) * kBytesPerPixel;
    return stride64 <= std::numeric_limits<uint32_t>::max() &&
           checked_frame_layout(static_cast<uint32_t>(rect.width),
                                static_cast<uint32_t>(rect.height), static_cast<uint32_t>(stride64),
                                &ignored);
}

float clamp_channel(float value) {
    return std::clamp(value, 0.0F, 255.0F);
}

uint8_t to_byte(float value) {
    return static_cast<uint8_t>(std::lround(clamp_channel(value)));
}

uint8_t premultiply(uint8_t channel, uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<uint32_t>(channel) * alpha + 127U) / 255U);
}

float distance_to_grid_line(float coordinate, float spacing) {
    float value = std::fmod(std::fabs(coordinate), spacing);
    if (value < 0.0F) {
        value += spacing;
    }
    return std::min(value, spacing - value);
}

float elliptical_falloff(float nx, float ny, float center_x, float center_y,
                         float radius_x, float radius_y) {
    const float dx = (nx - center_x) / radius_x;
    const float dy = (ny - center_y) / radius_y;
    float value = std::clamp(1.0F - dx * dx - dy * dy, 0.0F, 1.0F);
    value *= value;
    return value * (3.0F - 2.0F * value);
}

uint32_t atmosphere_hash(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t value = x * 0x9e3779b9U ^ y * 0x85ebca6bU ^ width * 0xc2b2ae35U ^ height;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    return value;
}

struct GlassPixel {
    float b{};
    float g{};
    float r{};
    float a{};
};

void apply_sao_glass_details(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                             GlassPixel* pixel) {
    const float half_width = static_cast<float>(width) * 0.5F;
    const float half_height = static_cast<float>(height) * 0.5F;
    const float nx = (static_cast<float>(x) + 0.5F - half_width) / std::max(half_width, 1.0F);
    const float ny = (static_cast<float>(y) + 0.5F - half_height) / std::max(half_height, 1.0F);
    const float radius_sq = nx * nx + ny * ny;
    const float radius = std::sqrt(radius_sq);
    const float center = std::clamp(1.0F - radius * 0.72F, 0.0F, 1.0F);
    const float vignette = std::clamp((radius - 0.48F) / 0.78F, 0.0F, 1.0F);
    const float lens_ring = std::clamp(1.0F - std::fabs(radius - 0.72F) / 0.028F, 0.0F, 1.0F);

    const float cyan_glow = elliptical_falloff(nx, ny, -0.58F, -0.34F, 1.05F, 0.92F);
    const float gold_glow = elliptical_falloff(nx, ny, 0.72F, 0.52F, 0.90F, 0.84F);
    pixel->b += cyan_glow * static_cast<float>(kGlassCyan.b) * 0.052F +
                gold_glow * static_cast<float>(kGlassGold.b) * 0.024F;
    pixel->g += cyan_glow * static_cast<float>(kGlassCyan.g) * 0.038F +
                gold_glow * static_cast<float>(kGlassGold.g) * 0.032F;
    pixel->r += cyan_glow * static_cast<float>(kGlassCyan.r) * 0.012F +
                gold_glow * static_cast<float>(kGlassGold.r) * 0.038F;
    pixel->a += cyan_glow * 4.0F + gold_glow * 3.0F;

    pixel->b += center * 13.0F + lens_ring * 18.0F;
    pixel->g += center * 8.0F + lens_ring * 24.0F;
    pixel->r += center * 2.0F + lens_ring * 4.0F;
    pixel->a += center * 8.0F - vignette * 18.0F + lens_ring * 5.0F;

    const float barrel = 1.0F + 0.30F * radius_sq;
    const float warped_x = (nx * barrel * 0.5F + 0.5F) * static_cast<float>(width);
    const float warped_y = (ny * barrel * 0.5F + 0.5F) * static_cast<float>(height);
    const float grid_spacing =
        std::clamp(static_cast<float>(std::min(width, height)) / 8.0F, 9.0F, 24.0F);
    const bool grid_line = distance_to_grid_line(warped_x, grid_spacing) < 0.65F ||
                           distance_to_grid_line(warped_y, grid_spacing) < 0.65F;
    if (grid_line) {
        const float horizon_fade = std::clamp(1.0F - std::fabs(ny + 0.10F) * 0.72F,
                                              0.18F, 1.0F);
        pixel->b += 5.0F * horizon_fade;
        pixel->g += 6.0F * horizon_fade;
        pixel->r += 1.0F * horizon_fade;
        pixel->a += 2.0F;
    }

    const float lane_distance = std::fabs(ny - (0.34F * nx + 0.18F));
    if (lane_distance < 0.006F) {
        const float lane_fade = std::clamp(1.0F - radius * 0.58F, 0.0F, 1.0F);
        pixel->b += 7.0F * lane_fade;
        pixel->g += 6.0F * lane_fade;
        pixel->r += 2.0F * lane_fade;
        pixel->a += 2.0F * lane_fade;
    }

    const uint32_t node_hash = atmosphere_hash(x, y, width, height);
    if ((node_hash & 0x7ffU) == 0U && radius_sq < 1.58F) {
        const bool warm = (node_hash & 0x800U) != 0U;
        const auto color = warm ? kGlassGold : kGlassCyan;
        pixel->b += static_cast<float>(color.b) * 0.18F;
        pixel->g += static_cast<float>(color.g) * 0.18F;
        pixel->r += static_cast<float>(color.r) * 0.18F;
        pixel->a += 16.0F;
    }

    if ((y + ((x / 13U) & 1U)) % 4U == 0U) {
        pixel->b -= 5.0F;
        pixel->g -= 4.0F;
        pixel->r -= 2.0F;
        pixel->a -= 3.0F;
    }

    const uint32_t border = std::max<uint32_t>(1U, std::min(width, height) / 96U);
    const bool on_border = x < border || y < border || x >= width - border || y >= height - border;
    if (on_border) {
        pixel->b = std::max(pixel->b, static_cast<float>(kGlassBorder.b));
        pixel->g = std::max(pixel->g, static_cast<float>(kGlassBorder.g));
        pixel->r = std::max(pixel->r, static_cast<float>(kGlassBorder.r));
        pixel->a = std::max(pixel->a, 232.0F);
    }

    const uint32_t accent_length = std::clamp<uint32_t>(std::min(width, height) / 5U, 5U, 34U);
    const uint32_t accent_width = std::min(accent_length, width);
    const uint32_t accent_height = std::min(accent_length, height);
    const uint32_t accent_thickness =
        std::min({std::clamp<uint32_t>(border + 1U, 2U, 3U), width, height});
    const bool cyan_top_left =
        (y < accent_thickness && x < accent_width) || (x < accent_thickness && y < accent_height);
    const bool cyan_bottom_right = (y >= height - accent_thickness && x >= width - accent_width) ||
                                   (x >= width - accent_thickness && y >= height - accent_height);
    const bool gold_top_right = (y < accent_thickness && x >= width - accent_width) ||
                                (x >= width - accent_thickness && y < accent_height);
    const bool gold_bottom_left = (y >= height - accent_thickness && x < accent_width) ||
                                  (x < accent_thickness && y >= height - accent_height);

    if (cyan_top_left || cyan_bottom_right) {
        pixel->b = static_cast<float>(kGlassCyan.b);
        pixel->g = static_cast<float>(kGlassCyan.g);
        pixel->r = static_cast<float>(kGlassCyan.r);
        pixel->a = 246.0F;
    } else if (gold_top_right || gold_bottom_left) {
        pixel->b = static_cast<float>(kGlassGold.b);
        pixel->g = static_cast<float>(kGlassGold.g);
        pixel->r = static_cast<float>(kGlassGold.r);
        pixel->a = 248.0F;
    }
}

void write_premultiplied_pixel(uint8_t* destination, const GlassPixel& source) {
    const uint8_t alpha = to_byte(source.a);
    destination[0] = premultiply(to_byte(source.b), alpha);
    destination[1] = premultiply(to_byte(source.g), alpha);
    destination[2] = premultiply(to_byte(source.r), alpha);
    destination[3] = alpha;
}

sao_status_t render_procedural_impl(uint32_t width, uint32_t height, uint32_t stride,
                                    uint8_t* out_bgra, size_t capacity,
                                    size_t* out_required_bytes) {
    if (out_required_bytes == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_required_bytes = 0;
    size_t required = 0;
    if (!checked_frame_layout(width, height, stride, &required)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_required_bytes = required;
    if (out_bgra == nullptr) {
        return capacity == 0 ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (capacity < required) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }

    std::memset(out_bgra, 0, required);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = out_bgra + static_cast<size_t>(y) * stride;
        const float vertical =
            static_cast<float>(y) / static_cast<float>(std::max<uint32_t>(height - 1U, 1U));
        for (uint32_t x = 0; x < width; ++x) {
            const float horizontal =
                static_cast<float>(x) / static_cast<float>(std::max<uint32_t>(width - 1U, 1U));
            GlassPixel pixel{
                static_cast<float>(kGlassBackground.b) + 15.0F * (1.0F - vertical) +
                    4.0F * horizontal,
                static_cast<float>(kGlassBackground.g) + 10.0F * (1.0F - vertical),
                static_cast<float>(kGlassBackground.r) + 3.0F * horizontal,
                static_cast<float>(kGlassBackground.a) + 9.0F * (1.0F - vertical),
            };
            apply_sao_glass_details(x, y, width, height, &pixel);
            write_premultiplied_pixel(row + static_cast<size_t>(x) * kBytesPerPixel, pixel);
        }
    }
    return SAO_STATUS_OK;
}

void rotate_source_coordinate(uint32_t rotation, int64_t local_x, int64_t local_y,
                              uint32_t source_width, uint32_t source_height, uint32_t* out_x,
                              uint32_t* out_y) {
    int64_t source_x = local_x;
    int64_t source_y = local_y;
    switch (rotation) {
    case 2:
        source_x = local_y;
        source_y = static_cast<int64_t>(source_height) - 1 - local_x;
        break;
    case 3:
        source_x = static_cast<int64_t>(source_width) - 1 - local_x;
        source_y = static_cast<int64_t>(source_height) - 1 - local_y;
        break;
    case 4:
        source_x = static_cast<int64_t>(source_width) - 1 - local_y;
        source_y = local_x;
        break;
    default:
        break;
    }
    source_x = std::clamp<int64_t>(source_x, 0, source_width - 1U);
    source_y = std::clamp<int64_t>(source_y, 0, source_height - 1U);
    *out_x = static_cast<uint32_t>(source_x);
    *out_y = static_cast<uint32_t>(source_y);
}

sao_status_t render_live_impl(const uint8_t* source_bgra, size_t source_bytes,
                              uint32_t source_width, uint32_t source_height, uint32_t source_stride,
                              int32_t source_origin_x, int32_t source_origin_y, uint32_t rotation,
                              const SaoUiFisheyeBackdropRect& desktop_rect,
                              std::vector<uint8_t>* out_frame) {
    if (source_bgra == nullptr || out_frame == nullptr || !valid_rect(desktop_rect)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    size_t checked_source_bytes = 0;
    if (!checked_frame_layout(source_width, source_height, source_stride, &checked_source_bytes) ||
        source_bytes < checked_source_bytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const uint32_t width = static_cast<uint32_t>(desktop_rect.width);
    const uint32_t height = static_cast<uint32_t>(desktop_rect.height);
    const uint32_t stride = width * kBytesPerPixel;
    size_t output_bytes = 0;
    if (!checked_frame_layout(width, height, stride, &output_bytes)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        out_frame->assign(output_bytes, 0);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    const float half_width = static_cast<float>(width) * 0.5F;
    const float half_height = static_cast<float>(height) * 0.5F;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* output_row = out_frame->data() + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const float nx =
                (static_cast<float>(x) + 0.5F - half_width) / std::max(half_width, 1.0F);
            const float ny =
                (static_cast<float>(y) + 0.5F - half_height) / std::max(half_height, 1.0F);
            const float radius_sq = nx * nx + ny * ny;
            const float lens_scale = 1.0F - 0.24F * std::clamp(radius_sq, 0.0F, 1.55F);
            const double target_x = static_cast<double>(desktop_rect.x) +
                                    (static_cast<double>(nx * lens_scale) * 0.5 + 0.5) *
                                        static_cast<double>(desktop_rect.width - 1);
            const double target_y = static_cast<double>(desktop_rect.y) +
                                    (static_cast<double>(ny * lens_scale) * 0.5 + 0.5) *
                                        static_cast<double>(desktop_rect.height - 1);
            const int64_t local_x = static_cast<int64_t>(std::llround(target_x)) - source_origin_x;
            const int64_t local_y = static_cast<int64_t>(std::llround(target_y)) - source_origin_y;
            uint32_t source_x = 0;
            uint32_t source_y = 0;
            rotate_source_coordinate(rotation, local_x, local_y, source_width, source_height,
                                     &source_x, &source_y);
            const uint8_t* source = source_bgra + static_cast<size_t>(source_y) * source_stride +
                                    static_cast<size_t>(source_x) * kBytesPerPixel;

            const float luma = 0.114F * source[0] + 0.587F * source[1] + 0.299F * source[2];
            GlassPixel pixel{
                source[0] * 0.48F + 32.0F + luma * 0.05F,
                source[1] * 0.40F + 16.0F + luma * 0.03F,
                source[2] * 0.31F + 5.0F,
                216.0F,
            };
            apply_sao_glass_details(x, y, width, height, &pixel);
            write_premultiplied_pixel(output_row + static_cast<size_t>(x) * kBytesPerPixel, pixel);
        }
    }
    return SAO_STATUS_OK;
}

} // namespace

struct sao_ui_fisheye_backdrop_s {
    sao_ui_compositor_handle_t compositor{nullptr};
    std::thread::id owner_thread{};
    std::string layer_name;

    std::mutex mutex;
    std::condition_variable_any worker_cv;
    bool destroying{false};
    bool desired_visible{false};
    SaoUiFisheyeBackdropMode desired_mode{SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL};
    BackdropGeometry desired_geometry{};
    uint64_t desired_revision{1};

    sao_ui_layer_handle_t layer{nullptr};
    bool layer_has_frame{false};
    bool applied_visible{false};
    SaoUiFisheyeBackdropMode applied_mode{SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL};
    BackdropGeometry applied_geometry{};
    uint64_t applied_revision{0};
    sao::ui::fisheye_gpu::Renderer* gpu{};
    bool gpu_bound{};
    bool fade_target{};
    float opacity{};
    float fade_from{};
    uint32_t fade_elapsed{};
    uint64_t visual_ms{};
    std::chrono::steady_clock::time_point last_tick{std::chrono::steady_clock::now()};

    bool live_available{false};
    sao_status_t last_status{SAO_STATUS_OK};
    uint64_t frame_generation{0};
    bool live_worker_running{false};
    bool live_resources_active{false};

    bool worker_report_pending{false};
    bool worker_report_live_available{false};
    sao_status_t worker_report_status{SAO_STATUS_ERR_NOT_INITIALIZED};
    uint64_t worker_report_revision{0};
    uint64_t worker_report_frames{0};

    std::jthread worker;
};

namespace {

sao_status_t desktop_capture_rect(sao_ui_fisheye_backdrop_s* backdrop,
                                  const SaoUiFisheyeBackdropRect& host_local_rect,
                                  SaoUiFisheyeBackdropRect* out_desktop_rect) {
    if (out_desktop_rect == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_desktop_rect = host_local_rect;
    if (backdrop->compositor == nullptr) {
        return SAO_STATUS_OK;
    }
    const sao_ui_overlay_host_handle_t host = sao_ui_compositor_host(backdrop->compositor);
    if (host == nullptr) {
        return SAO_STATUS_OK;
    }

    SaoOverlayHostClientRect host_rect{};
    const sao_status_t host_status = sao_ui_overlay_host_get_client_rect(host, &host_rect);
    if (host_status != SAO_STATUS_OK) {
        return host_status;
    }
    // DPI-aware: host_local_rect is in host logical pixels, host_rect origin
    // is in desktop physical pixels. Scale the local extent up by host_dpi/96
    // before adding to the desktop origin so DXGI capture reads the correct
    // physical rectangle on 120/144 DPI monitors. host_dpi == 96 is a no-op.
    const uint32_t host_dpi = sao_ui_overlay_host_current_dpi(host);
    const uint32_t dpi = host_dpi == 0u ? 96u : host_dpi;
    int64_t local_x = host_local_rect.x;
    int64_t local_y = host_local_rect.y;
    int64_t local_w = host_local_rect.width;
    int64_t local_h = host_local_rect.height;
    if (dpi != 96u) {
        local_x = local_x * static_cast<int64_t>(dpi) / static_cast<int64_t>(96);
        local_y = local_y * static_cast<int64_t>(dpi) / static_cast<int64_t>(96);
        local_w = local_w * static_cast<int64_t>(dpi) / static_cast<int64_t>(96);
        local_h = local_h * static_cast<int64_t>(dpi) / static_cast<int64_t>(96);
    }
    const int64_t desktop_x = static_cast<int64_t>(host_rect.x) + local_x;
    const int64_t desktop_y = static_cast<int64_t>(host_rect.y) + local_y;
    if (desktop_x < std::numeric_limits<int32_t>::min() ||
        desktop_x > std::numeric_limits<int32_t>::max() ||
        desktop_y < std::numeric_limits<int32_t>::min() ||
        desktop_y > std::numeric_limits<int32_t>::max() ||
        local_w < 0 || local_h < 0 ||
        local_w > std::numeric_limits<int32_t>::max() ||
        local_h > std::numeric_limits<int32_t>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    out_desktop_rect->x = static_cast<int32_t>(desktop_x);
    out_desktop_rect->y = static_cast<int32_t>(desktop_y);
    out_desktop_rect->width = static_cast<int32_t>(local_w);
    out_desktop_rect->height = static_cast<int32_t>(local_h);
    return valid_rect(*out_desktop_rect) ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

void destroy_live_resources(sao_ui_fisheye_backdrop_s* backdrop,
                            sao_ui_dxgi_dup_handle_t* duplication) {
    sao_ui_dxgi_dup_destroy(*duplication);
    *duplication = nullptr;
    std::lock_guard lock(backdrop->mutex);
    backdrop->live_resources_active = false;
}

sao_status_t stop_and_join_live_worker(sao_ui_fisheye_backdrop_s* backdrop) {
    if (!backdrop->worker.joinable()) {
        std::lock_guard lock(backdrop->mutex);
        backdrop->live_worker_running = false;
        backdrop->live_resources_active = false;
        return SAO_STATUS_OK;
    }
    backdrop->worker.request_stop();
    backdrop->worker_cv.notify_all();
    try {
        backdrop->worker.join();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    std::lock_guard lock(backdrop->mutex);
    backdrop->live_worker_running = false;
    backdrop->live_resources_active = false;
    return SAO_STATUS_OK;
}

sao_status_t reap_live_worker_if_inactive(sao_ui_fisheye_backdrop_s* backdrop) {
    bool should_reap = false;
    {
        std::lock_guard lock(backdrop->mutex);
        const bool live_requested = backdrop->desired_visible &&
                                    backdrop->desired_mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE;
        should_reap =
            backdrop->worker.joinable() && (!live_requested || !backdrop->live_worker_running);
    }
    if (!should_reap) {
        return SAO_STATUS_OK;
    }
    const sao_status_t status = stop_and_join_live_worker(backdrop);
    if (status != SAO_STATUS_OK) {
        return status;
    }
    std::lock_guard lock(backdrop->mutex);
    if (!backdrop->desired_visible || backdrop->desired_mode != SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) {
        backdrop->live_available = false;
        backdrop->worker_report_pending = false;
        backdrop->worker_report_frames = 0;
    }
    return SAO_STATUS_OK;
}

void publish_worker_report(sao_ui_fisheye_backdrop_s* backdrop, uint64_t revision,
                           sao_status_t status, bool live_available, bool frame_published) {
    std::lock_guard lock(backdrop->mutex);
    if (backdrop->destroying) {
        return;
    }
    if (!backdrop->worker_report_pending || backdrop->worker_report_revision != revision) {
        backdrop->worker_report_frames = 0;
    }
    backdrop->worker_report_pending = true;
    backdrop->worker_report_revision = revision;
    backdrop->worker_report_status = status;
    backdrop->worker_report_live_available = live_available;
    if (frame_published) {
        ++backdrop->worker_report_frames;
    }
}

void consume_worker_report_locked(sao_ui_fisheye_backdrop_s* backdrop) {
    if (!backdrop->worker_report_pending) {
        return;
    }
    if (backdrop->desired_mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE &&
        backdrop->worker_report_revision == backdrop->desired_revision) {
        backdrop->last_status = backdrop->worker_report_status;
        backdrop->live_available = backdrop->worker_report_live_available;
        backdrop->frame_generation += backdrop->worker_report_frames;
    }
    backdrop->worker_report_pending = false;
    backdrop->worker_report_frames = 0;
}

bool wait_for_live_work(sao_ui_fisheye_backdrop_s* backdrop, std::stop_token stop_token,
                        BackdropGeometry* out_geometry, sao_ui_layer_handle_t* out_layer,
                        uint64_t* out_revision) {
    std::unique_lock lock(backdrop->mutex);
    const bool ready = backdrop->worker_cv.wait(lock, stop_token, [&] {
        return backdrop->destroying || !backdrop->desired_visible ||
               backdrop->desired_mode != SAO_UI_FISHEYE_BACKDROP_MODE_LIVE ||
               (backdrop->desired_visible &&
                backdrop->desired_mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE &&
                backdrop->layer != nullptr && valid_rect(backdrop->desired_geometry.rect));
    });
    if (!ready || stop_token.stop_requested() || backdrop->destroying ||
        !backdrop->desired_visible || backdrop->desired_mode != SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) {
        return false;
    }
    *out_geometry = backdrop->desired_geometry;
    *out_layer = backdrop->layer;
    *out_revision = backdrop->desired_revision;
    return true;
}

sao_status_t upload_live_if_current(sao_ui_fisheye_backdrop_s* backdrop,
                                    const BackdropGeometry& geometry, sao_ui_layer_handle_t layer,
                                    uint64_t revision, const std::vector<uint8_t>& frame) {
    std::lock_guard lock(backdrop->mutex);
    if (backdrop->destroying || !backdrop->desired_visible ||
        backdrop->desired_mode != SAO_UI_FISHEYE_BACKDROP_MODE_LIVE ||
        backdrop->desired_revision != revision || backdrop->layer != layer ||
        !same_geometry(backdrop->desired_geometry, geometry)) {
        return SAO_STATUS_ERR_CANCELLED;
    }
    const uint32_t width = static_cast<uint32_t>(geometry.rect.width);
    const uint32_t height = static_cast<uint32_t>(geometry.rect.height);
    return sao_ui_layer_update_bgra(layer, frame.data(), width, height, width * kBytesPerPixel);
}

void wait_live_retry(sao_ui_fisheye_backdrop_s* backdrop, std::stop_token stop_token,
                     uint64_t revision, std::chrono::milliseconds delay) {
    std::unique_lock lock(backdrop->mutex);
    (void)backdrop->worker_cv.wait_for(lock, stop_token, delay, [&] {
        return backdrop->destroying || backdrop->desired_revision != revision ||
               !backdrop->desired_visible ||
               backdrop->desired_mode != SAO_UI_FISHEYE_BACKDROP_MODE_LIVE;
    });
}

sao_status_t copy_held_dxgi_frame(sao_ui_dxgi_dup_handle_t duplication,
                                  const SaoDxgiDupFrame& frame, std::vector<uint8_t>* out_source,
                                  uint32_t* out_stride) {
    const uint64_t tight_stride64 = static_cast<uint64_t>(frame.width) * kBytesPerPixel;
    if (tight_stride64 > std::numeric_limits<uint32_t>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    size_t tight_bytes = 0;
    if (!checked_frame_layout(frame.width, frame.height, static_cast<uint32_t>(tight_stride64),
                              &tight_bytes) ||
        tight_bytes > std::numeric_limits<uint32_t>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        out_source->assign(tight_bytes, 0);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    uint32_t stride = 0;
    uint32_t written = 0;
    sao_status_t status = sao_ui_dxgi_dup_copy_to_staging(duplication, out_source->data(),
                                                          static_cast<uint32_t>(out_source->size()),
                                                          &stride, &written);
    if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
        if (written == 0 || written > kMaxFrameBytes) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        try {
            out_source->assign(written, 0);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        status = sao_ui_dxgi_dup_copy_to_staging(duplication, out_source->data(),
                                                 static_cast<uint32_t>(out_source->size()), &stride,
                                                 &written);
    }
    if (status != SAO_STATUS_OK) {
        return status;
    }
    size_t checked_bytes = 0;
    if (!checked_frame_layout(frame.width, frame.height, stride, &checked_bytes) ||
        written < checked_bytes || out_source->size() < checked_bytes) {
        return SAO_STATUS_ERR_SURFACE_INVALID;
    }
    *out_stride = stride;
    return SAO_STATUS_OK;
}

void live_worker(std::stop_token stop_token, sao_ui_fisheye_backdrop_s* backdrop) noexcept {
    sao_ui_dxgi_dup_handle_t duplication = nullptr;
    std::vector<uint8_t> source;
    std::vector<uint8_t> warped;
    uint64_t previous_revision = 0;
    bool has_current_live_frame = false;

    try {
        while (!stop_token.stop_requested()) {
            BackdropGeometry geometry{};
            sao_ui_layer_handle_t layer = nullptr;
            uint64_t revision = 0;
            if (!wait_for_live_work(backdrop, stop_token, &geometry, &layer, &revision)) {
                break;
            }
            if (previous_revision != revision) {
                previous_revision = revision;
                has_current_live_frame = false;
            }

            if (duplication == nullptr) {
                SaoDxgiDupConfig config{};
                config.output_index = 0;
                config.adapter_index = 0;
                config.acquire_timeout_ms = kLiveAcquireTimeoutMs;
                config.auto_recover = true;
                const sao_status_t create_status = sao_ui_dxgi_dup_create(&config, &duplication);
                if (create_status != SAO_STATUS_OK) {
                    publish_worker_report(backdrop, revision, create_status, false, false);
                    wait_live_retry(backdrop, stop_token, revision, kLiveCreateRetryDelay);
                    continue;
                }
                std::lock_guard lock(backdrop->mutex);
                backdrop->live_resources_active = true;
            }
            if (stop_token.stop_requested()) {
                break;
            }

            SaoDxgiDupFrame frame{};
            const sao_status_t acquire_status = sao_ui_dxgi_dup_acquire_frame(duplication, &frame);
            if (acquire_status == SAO_STATUS_ERR_TIMEOUT) {
                if (!has_current_live_frame) {
                    publish_worker_report(backdrop, revision, acquire_status, false, false);
                }
                continue;
            }
            if (acquire_status != SAO_STATUS_OK) {
                has_current_live_frame = false;
                publish_worker_report(backdrop, revision, acquire_status, false, false);
                if (acquire_status == SAO_STATUS_ERR_DEVICE_LOST) {
                    destroy_live_resources(backdrop, &duplication);
                }
                wait_live_retry(backdrop, stop_token, revision, kLiveRetryDelay);
                continue;
            }

            uint32_t source_stride = 0;
            sao_status_t status = SAO_STATUS_OK;
            if (frame.last_present_time == 0 || frame.width == 0 || frame.height == 0) {
                status = SAO_STATUS_ERR_TIMEOUT;
            } else {
                status = copy_held_dxgi_frame(duplication, frame, &source, &source_stride);
            }
            const sao_status_t release_status = sao_ui_dxgi_dup_release_frame(duplication);
            if (status == SAO_STATUS_OK && release_status != SAO_STATUS_OK) {
                status = release_status;
            }
            SaoUiFisheyeBackdropRect capture_rect{};
            if (status == SAO_STATUS_OK) {
                status = desktop_capture_rect(backdrop, geometry.rect, &capture_rect);
            }
            if (status == SAO_STATUS_OK) {
                status = render_live_impl(source.data(), source.size(), frame.width, frame.height,
                                          source_stride, frame.origin_x, frame.origin_y,
                                          frame.rotation, capture_rect, &warped);
            }
            if (status == SAO_STATUS_OK) {
                status = upload_live_if_current(backdrop, geometry, layer, revision, warped);
            }
            if (status == SAO_STATUS_ERR_CANCELLED) {
                continue;
            }

            if (status == SAO_STATUS_OK) {
                has_current_live_frame = true;
                publish_worker_report(backdrop, revision, SAO_STATUS_OK, true, true);
            } else {
                has_current_live_frame = false;
                publish_worker_report(backdrop, revision, status, false, false);
                wait_live_retry(backdrop, stop_token, revision, kLiveRetryDelay);
            }
        }
    } catch (...) {
        if (previous_revision != 0) {
            publish_worker_report(backdrop, previous_revision, SAO_STATUS_ERR_UNKNOWN, false,
                                  false);
        }
    }

    destroy_live_resources(backdrop, &duplication);
    std::lock_guard lock(backdrop->mutex);
    backdrop->live_worker_running = false;
    backdrop->live_available = false;
}

sao_status_t render_procedural_vector(const SaoUiFisheyeBackdropRect& rect,
                                      std::vector<uint8_t>* out_frame) {
    const uint32_t width = static_cast<uint32_t>(rect.width);
    const uint32_t height = static_cast<uint32_t>(rect.height);
    const uint32_t stride = width * kBytesPerPixel;
    size_t required = 0;
    if (!checked_frame_layout(width, height, stride, &required)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        out_frame->assign(required, 0);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return render_procedural_impl(width, height, stride, out_frame->data(), out_frame->size(),
                                  &required);
}

#if defined(_WIN32)
sao_status_t tick_procedural(sao_ui_fisheye_backdrop_s* handle, const BackdropGeometry& geometry,
                              bool visible, uint64_t revision, uint32_t delta_ms) {
    const bool reduced = sao_ui_reduced_motion_enabled();
    float opacity = 0.0F;
    float seconds = 0.0F;
    sao_ui_layer_handle_t layer = nullptr;
    {
        std::lock_guard lock(handle->mutex);
        if (visible != handle->fade_target) {
            handle->fade_target = visible;
            handle->fade_from = handle->opacity;
            handle->fade_elapsed = 0;
            if (visible && handle->opacity <= 0.001F)
                handle->visual_ms = 0;
        }
        handle->visual_ms += delta_ms;
        const uint32_t duration = visible ? 500u : 400u;
        handle->fade_elapsed = std::min(duration, handle->fade_elapsed + delta_ms);
        float t = reduced ? 1.0F : static_cast<float>(handle->fade_elapsed) / static_cast<float>(duration);
        t = t * t * (3.0F - 2.0F * t);
        handle->opacity = handle->fade_from + ((visible ? 1.0F : 0.0F) - handle->fade_from) * t;
        opacity = handle->opacity;
        seconds = static_cast<float>(handle->visual_ms) / 1000.0F;
        layer = handle->layer;
    }
    if (!layer && !visible)
        return SAO_STATUS_OK;
    if (!layer) {
        SaoLayerConfig config{};
        config.struct_size = sizeof(config);
        config.name_utf8 = handle->layer_name.c_str();
        config.x = geometry.rect.x; config.y = geometry.rect.y;
        config.width = geometry.rect.width; config.height = geometry.rect.height;
        config.z_order = geometry.z_order;
        config.click_through = true; config.bgra_swizzle = true;
        config.high_fps = true; config.target_fps = 60;
        const auto status = sao_ui_layer_create(handle->compositor, &config, &layer);
        if (status != SAO_STATUS_OK) return status;
        std::lock_guard lock(handle->mutex);
        handle->layer = layer;
    }
    if (!handle->gpu) {
        const auto status = sao::ui::fisheye_gpu::create(&handle->gpu);
        if (status != SAO_STATUS_OK) return status;
    }
    sao::ui::fisheye_gpu::update(handle->gpu, seconds, opacity, reduced);
    sao_status_t status = sao_ui_layer_set_geometry(layer, geometry.rect.x, geometry.rect.y,
                                                    geometry.rect.width, geometry.rect.height);
    if (status == SAO_STATUS_OK) status = sao_ui_layer_set_z_order(layer, geometry.z_order);
    if (status == SAO_STATUS_OK && !handle->gpu_bound) {
        status = sao_ui_layer_set_d3d11_render_fn(layer, sao::ui::fisheye_gpu::render, handle->gpu);
        if (status == SAO_STATUS_OK) handle->gpu_bound = true;
    }
    if (status == SAO_STATUS_OK) status = sao_ui_layer_set_visible(layer, visible || opacity > 0.001F);
    if (status == SAO_STATUS_OK && (visible || opacity > 0.001F))
        status = sao_ui_layer_request_redraw(layer);
    {
        std::lock_guard lock(handle->mutex);
        handle->last_status = status;
        if (status == SAO_STATUS_OK && handle->desired_revision == revision) {
            handle->layer_has_frame = true;
            handle->applied_visible = visible || opacity > 0.001F;
            handle->applied_mode = SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL;
            handle->applied_geometry = geometry;
            handle->applied_revision = revision;
            handle->live_available = false;
            ++handle->frame_generation;
        }
    }
    return status;
}
#endif

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_create(
    sao_ui_compositor_handle_t compositor, sao_ui_fisheye_backdrop_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (compositor != nullptr) {
        const sao_status_t owner_status = sao_ui_compositor_require_owner_thread(compositor);
        if (owner_status != SAO_STATUS_OK) {
            return owner_status;
        }
    }
    try {
        auto backdrop = std::make_unique<sao_ui_fisheye_backdrop_s>();
        backdrop->compositor = compositor;
        backdrop->owner_thread = std::this_thread::get_id();
        const uint64_t sequence =
            g_backdrop_name_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        backdrop->layer_name = "sao.ui.fisheye_backdrop." + std::to_string(sequence);
        *out_handle = backdrop.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_try_destroy(sao_ui_fisheye_backdrop_handle_t handle) {
    if (handle == nullptr) {
        return SAO_STATUS_OK;
    }
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (handle->compositor != nullptr) {
        const sao_status_t owner_status =
            sao_ui_compositor_require_owner_thread(handle->compositor);
        if (owner_status != SAO_STATUS_OK) {
            return owner_status;
        }
    }

    sao_ui_layer_handle_t layer = nullptr;
    {
        std::lock_guard lock(handle->mutex);
        if (handle->destroying) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        handle->destroying = true;
        layer = handle->layer;
        handle->layer = nullptr;
    }
    const sao_status_t worker_status = stop_and_join_live_worker(handle);
    if (worker_status != SAO_STATUS_OK) {
        std::lock_guard lock(handle->mutex);
        handle->destroying = false;
        handle->layer = layer;
        return worker_status;
    }

    if (layer != nullptr && handle->gpu_bound) {
        const auto status = sao_ui_layer_set_d3d11_render_fn(layer, nullptr, nullptr);
        if (status != SAO_STATUS_OK) {
            std::lock_guard lock(handle->mutex);
            handle->destroying = false;
            handle->layer = layer;
            return status;
        }
    }
    sao_ui_layer_destroy(layer);
    sao::ui::fisheye_gpu::destroy(handle->gpu);
    delete handle;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL
sao_ui_fisheye_backdrop_destroy(sao_ui_fisheye_backdrop_handle_t handle) {
    (void)sao_ui_fisheye_backdrop_try_destroy(handle);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_set_mode(
    sao_ui_fisheye_backdrop_handle_t handle, SaoUiFisheyeBackdropMode mode) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (mode != SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL &&
        mode != SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard lock(handle->mutex);
        if (handle->destroying) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        if (handle->desired_mode == mode) {
            return SAO_STATUS_OK;
        }
        handle->desired_mode = mode;
        ++handle->desired_revision;
        handle->live_available = false;
        handle->last_status = mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL
                                  ? SAO_STATUS_OK
                                  : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    handle->worker_cv.notify_all();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_get_mode(
    sao_ui_fisheye_backdrop_handle_t handle, SaoUiFisheyeBackdropMode* out_mode) {
    if (out_mode == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    std::lock_guard lock(handle->mutex);
    if (handle->destroying) {
        return SAO_STATUS_ERR_CANCELLED;
    }
    *out_mode = handle->desired_mode;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_show(sao_ui_fisheye_backdrop_handle_t handle,
                             const SaoUiFisheyeBackdropRect* rect, int32_t z_order) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (rect == nullptr || !valid_rect(*rect)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard lock(handle->mutex);
        if (handle->destroying) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        const BackdropGeometry requested{*rect, z_order};
        if (handle->desired_visible && same_geometry(handle->desired_geometry, requested)) {
            return SAO_STATUS_OK;
        }
        handle->desired_visible = true;
        handle->desired_geometry = requested;
        ++handle->desired_revision;
        if (handle->desired_mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) {
            handle->live_available = false;
            handle->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
        }
    }
    handle->worker_cv.notify_all();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_hide(sao_ui_fisheye_backdrop_handle_t handle) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    {
        std::lock_guard lock(handle->mutex);
        if (handle->destroying) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        if (!handle->desired_visible) {
            return SAO_STATUS_OK;
        }
        handle->desired_visible = false;
        ++handle->desired_revision;
    }
    handle->worker_cv.notify_all();
    return SAO_STATUS_OK;
}

static sao_status_t tick_backdrop(sao_ui_fisheye_backdrop_handle_t handle, uint32_t delta_ms) {
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (std::this_thread::get_id() != handle->owner_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    const sao_status_t worker_status = reap_live_worker_if_inactive(handle);
    if (worker_status != SAO_STATUS_OK) {
        std::lock_guard lock(handle->mutex);
        handle->last_status = worker_status;
        return worker_status;
    }

    BackdropGeometry geometry{};
    SaoUiFisheyeBackdropMode mode = SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL;
    sao_ui_layer_handle_t layer = nullptr;
    uint64_t revision = 0;
    bool visible = false;
    bool needs_apply = false;
    bool needs_frame = false;
    {
        std::lock_guard lock(handle->mutex);
        if (handle->destroying) {
            return SAO_STATUS_ERR_CANCELLED;
        }
        consume_worker_report_locked(handle);
        visible = handle->desired_visible;
        geometry = handle->desired_geometry;
        mode = handle->desired_mode;
        revision = handle->desired_revision;
        layer = handle->layer;
        needs_apply = handle->applied_revision != revision;
        needs_frame =
            visible &&
            (!handle->layer_has_frame || !same_rect(handle->applied_geometry.rect, geometry.rect) ||
             handle->applied_mode != mode || mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL);
    }

    if (handle->compositor == nullptr) {
        std::lock_guard lock(handle->mutex);
        handle->last_status = SAO_STATUS_ERR_CAPABILITY_MISSING;
        handle->live_available = false;
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }
    const sao_status_t owner_status = sao_ui_compositor_require_owner_thread(handle->compositor);
    if (owner_status != SAO_STATUS_OK) {
        std::lock_guard lock(handle->mutex);
        handle->last_status = owner_status;
        return owner_status;
    }

#if defined(_WIN32)
    if (mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL)
        return tick_procedural(handle, geometry, visible, revision, delta_ms);
    if (layer != nullptr && handle->gpu_bound) {
        const auto status = sao_ui_layer_set_d3d11_render_fn(layer, nullptr, nullptr);
        if (status != SAO_STATUS_OK) return status;
        handle->gpu_bound = false;
    }
#else
    (void)delta_ms;
#endif

    if (layer == nullptr && visible) {
        SaoLayerConfig config{};
        config.struct_size = sizeof(SaoLayerConfig);
        config.name_utf8 = handle->layer_name.c_str();
        config.x = geometry.rect.x;
        config.y = geometry.rect.y;
        config.width = geometry.rect.width;
        config.height = geometry.rect.height;
        config.z_order = geometry.z_order;
        config.click_through = true;
        config.rect_hit = false;
        config.bgra_swizzle = true;
        config.high_fps = true;
        config.target_fps = 60;
        const sao_status_t create_status = sao_ui_layer_create(handle->compositor, &config, &layer);
        if (create_status != SAO_STATUS_OK) {
            std::lock_guard lock(handle->mutex);
            handle->last_status = create_status;
            return create_status;
        }
        {
            std::lock_guard lock(handle->mutex);
            if (handle->destroying) {
                sao_ui_layer_destroy(layer);
                return SAO_STATUS_ERR_CANCELLED;
            }
            handle->layer = layer;
            handle->layer_has_frame = false;
            needs_apply = true;
            needs_frame = true;
        }
    }

    if (layer != nullptr && !visible && needs_apply) {
        const sao_status_t hide_status = sao_ui_layer_set_visible(layer, false);
        if (hide_status != SAO_STATUS_OK) {
            std::lock_guard lock(handle->mutex);
            handle->last_status = hide_status;
            return hide_status;
        }
        std::lock_guard lock(handle->mutex);
        if (handle->desired_revision == revision) {
            handle->applied_visible = false;
            handle->applied_revision = revision;
        }
        return SAO_STATUS_OK;
    }

    if (layer != nullptr && visible && needs_apply) {
        bool uploaded_procedural = false;
        if (needs_frame) {
            std::vector<uint8_t> frame;
            sao_status_t status = render_procedural_vector(geometry.rect, &frame);
            if (status == SAO_STATUS_OK) {
                const uint32_t width = static_cast<uint32_t>(geometry.rect.width);
                const uint32_t height = static_cast<uint32_t>(geometry.rect.height);
                status = sao_ui_layer_update_bgra(layer, frame.data(), width, height,
                                                  width * kBytesPerPixel);
            }
            if (status != SAO_STATUS_OK) {
                std::lock_guard lock(handle->mutex);
                handle->last_status = status;
                return status;
            }
            uploaded_procedural = true;
        }

        sao_status_t status = sao_ui_layer_set_geometry(layer, geometry.rect.x, geometry.rect.y,
                                                        geometry.rect.width, geometry.rect.height);
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_z_order(layer, geometry.z_order);
        }
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_visible(layer, true);
        }
        if (status != SAO_STATUS_OK) {
            std::lock_guard lock(handle->mutex);
            handle->last_status = status;
            return status;
        }

        {
            std::lock_guard lock(handle->mutex);
            if (handle->desired_revision == revision && handle->layer == layer) {
                handle->layer_has_frame = handle->layer_has_frame || uploaded_procedural;
                handle->applied_visible = true;
                handle->applied_mode = mode;
                handle->applied_geometry = geometry;
                handle->applied_revision = revision;
                if (uploaded_procedural) {
                    ++handle->frame_generation;
                }
                if (mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL) {
                    handle->last_status = SAO_STATUS_OK;
                    handle->live_available = false;
                } else if (handle->last_status == SAO_STATUS_OK && !handle->live_available) {
                    handle->last_status = SAO_STATUS_ERR_NOT_INITIALIZED;
                }
            }
        }
    }

    if (visible && mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) {
        try {
            if (!handle->worker.joinable()) {
                {
                    std::lock_guard lock(handle->mutex);
                    handle->live_worker_running = true;
                    handle->live_resources_active = false;
                }
                handle->worker = std::jthread(live_worker, handle);
            }
        } catch (...) {
            std::lock_guard lock(handle->mutex);
            handle->live_worker_running = false;
            handle->live_resources_active = false;
            handle->last_status = SAO_STATUS_ERR_UNKNOWN;
            handle->live_available = false;
            return SAO_STATUS_ERR_UNKNOWN;
        }
        handle->worker_cv.notify_all();
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_advance(sao_ui_fisheye_backdrop_handle_t handle, uint32_t delta_ms) {
    if (!handle) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) return SAO_STATUS_ERR_ACCESS_DENIED;
    if (delta_ms > 1000u) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        handle->last_tick = std::chrono::steady_clock::now();
        return tick_backdrop(handle, delta_ms);
    } catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_tick(sao_ui_fisheye_backdrop_handle_t handle) {
    if (!handle) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != handle->owner_thread) return SAO_STATUS_ERR_ACCESS_DENIED;
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - handle->last_tick).count();
    return sao_ui_fisheye_backdrop_advance(handle, static_cast<uint32_t>(std::clamp<int64_t>(delta, 0, 1000)));
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_fisheye_backdrop_service(sao_ui_fisheye_backdrop_handle_t handle) {
    return sao_ui_fisheye_backdrop_tick(handle);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_get_state(
    sao_ui_fisheye_backdrop_handle_t handle, SaoUiFisheyeBackdropState* out_state) {
    if (out_state == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_state = {};
    if (handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    std::lock_guard lock(handle->mutex);
    if (handle->destroying) {
        return SAO_STATUS_ERR_CANCELLED;
    }
    out_state->visible = handle->desired_visible;
    out_state->mode = handle->desired_mode;
    out_state->geometry.rect = handle->desired_geometry.rect;
    out_state->geometry.z_order = handle->desired_geometry.z_order;
    out_state->layer_present = handle->layer != nullptr;
    out_state->live_available = handle->live_available;
    out_state->last_status = handle->last_status;
    out_state->frame_generation = handle->frame_generation;
    out_state->live_worker_running = handle->live_worker_running;
    out_state->live_resources_active = handle->live_resources_active;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_fisheye_backdrop_render_procedural_bgra(
    uint32_t width, uint32_t height, uint32_t stride, uint8_t* out_bgra, size_t capacity,
    size_t* out_required_bytes) {
    try {
        return render_procedural_impl(width, height, stride, out_bgra, capacity,
                                      out_required_bytes);
    } catch (...) {
        if (out_required_bytes != nullptr) {
            *out_required_bytes = 0;
        }
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
