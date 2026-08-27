#include "sao/ui/compositor.h"
#include "sao/ui/overlay_host.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace {
void fill_white(uint8_t* p, uint32_t w, uint32_t h, uint32_t stride, float a) {
    const uint8_t v = static_cast<uint8_t>(a * 255.0f + 0.5f);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = p + y * stride;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = v; row[x * 4 + 1] = v;
            row[x * 4 + 2] = v; row[x * 4 + 3] = v;
        }
    }
}

struct FlashRequest { uint32_t hold_ms = 60; };

class LevelUpWorker {
public:
    ~LevelUpWorker() { bind(nullptr); }

    void bind(sao_ui_compositor_handle_t compositor) {
        std::thread old;
        {
            std::lock_guard<std::mutex> lock(mu_);
            compositor_ = nullptr;
            stop_ = true;
            pending_ = false;
            ++generation_;
            cv_.notify_all();
            old = std::move(worker_);
        }
        if (old.joinable()) {
            if (old.get_id() == std::this_thread::get_id()) old.detach();
            else old.join();
        }
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = false;
        compositor_ = compositor;
    }

    void flash(uint32_t duration_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        if (compositor_ == nullptr) return;
        FlashRequest request{};
        if (duration_ms > 240u) request.hold_ms = duration_ms - 240u;
        request_ = request;
        pending_ = true;
        ++generation_;
        if (!worker_.joinable()) {
            try { worker_ = std::thread([this] { run(); }); }
            catch (...) { pending_ = false; return; }
        }
        cv_.notify_all();
    }

private:
    bool cancelled(uint64_t gen, sao_ui_compositor_handle_t compositor) {
        std::lock_guard<std::mutex> lock(mu_);
        return stop_ || compositor_ != compositor || generation_ != gen;
    }

    bool wait(uint32_t ms, uint64_t gen, sao_ui_compositor_handle_t compositor) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(ms), [this, gen, compositor] {
            return stop_ || compositor_ != compositor || generation_ != gen;
        });
        return !stop_ && compositor_ == compositor && generation_ == gen;
    }

    bool size(sao_ui_compositor_handle_t compositor, uint32_t* w, uint32_t* h) {
        auto* host = sao_ui_compositor_host(compositor);
        SaoOverlayHostClientRect rect{};
        if (host == nullptr || sao_ui_overlay_host_get_client_rect(host, &rect) != SAO_STATUS_OK ||
            rect.width <= 0 || rect.height <= 0) return false;
        *w = static_cast<uint32_t>(rect.width);
        *h = static_cast<uint32_t>(rect.height);
        const uint64_t row = static_cast<uint64_t>(*w) * 4u;
        const uint64_t total = row * static_cast<uint64_t>(*h);
        return row <= std::numeric_limits<uint32_t>::max() &&
               total <= static_cast<uint64_t>(512u) * 1024u * 1024u;
    }

    bool create_layer(sao_ui_compositor_handle_t compositor, uint32_t width,
                      uint32_t height, sao_ui_layer_handle_t* out_layer) {
        if (out_layer == nullptr) return false;
        *out_layer = nullptr;
        static std::atomic<uint64_t> next_layer_serial{1u};
        char layer_name[64]{};
        std::snprintf(layer_name, sizeof(layer_name), "sao_ui_level_up_flash_%llu",
                      static_cast<unsigned long long>(next_layer_serial.fetch_add(1u)));
        SaoLayerConfig config{};
        config.struct_size = sizeof(config);
        config.name_utf8 = layer_name;
        config.width = static_cast<int32_t>(width);
        config.height = static_cast<int32_t>(height);
        config.z_order = std::numeric_limits<int32_t>::max();
        config.click_through = true;
        config.high_fps = true;
        return sao_ui_layer_create(compositor, &config, out_layer) == SAO_STATUS_OK &&
               *out_layer != nullptr;
    }

    bool update(sao_ui_layer_handle_t* layer, sao_ui_compositor_handle_t compositor,
                uint64_t gen, float alpha, std::vector<uint8_t>& pixels,
                uint32_t* width, uint32_t* height) {
        if (layer == nullptr || *layer == nullptr || width == nullptr || height == nullptr)
            return false;
        uint32_t next_width = 0;
        uint32_t next_height = 0;
        if (!size(compositor, &next_width, &next_height)) return false;
        if (next_width != *width || next_height != *height) {
            const uint64_t bytes = static_cast<uint64_t>(next_width) * next_height * 4u;
            std::vector<uint8_t> replacement_pixels;
            try { replacement_pixels.assign(static_cast<size_t>(bytes), 0u); }
            catch (...) { return false; }
            sao_ui_layer_handle_t replacement = nullptr;
            if (!create_layer(compositor, next_width, next_height, &replacement))
                return false;
            sao_ui_layer_handle_t old_layer = *layer;
            *layer = replacement;
            pixels.swap(replacement_pixels);
            *width = next_width;
            *height = next_height;
            sao_ui_layer_destroy(old_layer);
        }
        fill_white(pixels.data(), *width, *height, *width * 4u, alpha);
        return !cancelled(gen, compositor) &&
               sao_ui_layer_update_bgra(*layer, pixels.data(), *width, *height,
                                        *width * 4u) == SAO_STATUS_OK;
    }

    bool segment(sao_ui_layer_handle_t* layer, sao_ui_compositor_handle_t compositor,
                 uint64_t gen, uint32_t duration_ms, bool rising,
                 std::vector<uint8_t>& pixels, uint32_t* width, uint32_t* height) {
        const uint32_t frames = duration_ms / 16u;
        for (uint32_t i = 0; i <= frames; ++i) {
            const float progress = frames == 0u ? 1.0f : static_cast<float>(i) / frames;
            if (!update(layer, compositor, gen, rising ? progress : 1.0f - progress,
                        pixels, width, height)) return false;
            if (i != frames && !wait(16u, gen, compositor)) return false;
        }
        return true;
    }

    void run() {
        for (;;) {
            FlashRequest request{};
            sao_ui_compositor_handle_t compositor = nullptr;
            uint64_t gen = 0;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || pending_; });
                if (stop_) return;
                request = request_;
                pending_ = false;
                compositor = compositor_;
                gen = generation_;
            }
            uint32_t width = 0;
            uint32_t height = 0;
            if (!size(compositor, &width, &height)) continue;
            sao_ui_layer_handle_t layer = nullptr;
            if (!create_layer(compositor, width, height, &layer)) continue;
            std::vector<uint8_t> pixels;
            try { pixels.resize(static_cast<size_t>(width) * height * 4u); }
            catch (...) { sao_ui_layer_destroy(layer); continue; }
            bool ok = segment(&layer, compositor, gen, 40u, true, pixels, &width, &height);
            if (ok) ok = wait(request.hold_ms, gen, compositor);
            if (ok) ok = segment(&layer, compositor, gen, 200u, false, pixels, &width, &height);
            if (layer != nullptr) sao_ui_layer_destroy(layer);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    sao_ui_compositor_handle_t compositor_ = nullptr;
    FlashRequest request_{};
    uint64_t generation_ = 0;
    bool pending_ = false;
    bool stop_ = false;
};

LevelUpWorker* g_level_up_worker = nullptr;
void shutdown_level_up_worker() {
    if (g_level_up_worker != nullptr) g_level_up_worker->bind(nullptr);
}

LevelUpWorker& level_up_worker() {
    static LevelUpWorker* worker = [] {
        auto* value = new LevelUpWorker();
        g_level_up_worker = value;
        std::atexit(shutdown_level_up_worker);
        return value;
    }();
    return *worker;
}
} // namespace

extern "C" SAO_UI_API void SAO_UI_CALL sao_ui_level_up_effect_overlay_set_compositor(
    sao_ui_compositor_handle_t comp) {
    try {
        level_up_worker().bind(comp);
    } catch (...) {
    }
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_level_up_effect_overlay_flash(uint32_t duration_ms) {
    try {
        level_up_worker().flash(duration_ms);
    } catch (...) {
    }
}
