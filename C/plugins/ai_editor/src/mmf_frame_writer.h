#pragma once

// Writer side of the SOPF V1 MMF frame ring.
//
// Owns a MemoryMapped Section named `mmf_name`, publishes triple-buffered
// BGRA frames.  The main-process compositor is the reader via
// sao_ui_layer_set_mmf_source(); see platform/ui/include/sao/ui/compositor.h
// for the header layout this file mirrors.
//
// Threading: writer is single-threaded per instance (call from the UI /
// paint thread).  begin_frame / commit_frame form the atomic publish pair.
//
// dirty-aware: the writer only bumps `published_generation` when
// commit_frame() runs.  If the caller decides no repaint is needed (skip
// the whole begin/commit pair), the compositor sees the unchanged
// generation and does not re-upload -- zero overhead when the UI is idle.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "sao/ui/compositor.h"

namespace sao::ai_editor {

class MmfFrameWriter {
public:
    MmfFrameWriter() = default;
    ~MmfFrameWriter() { shutdown(); }

    MmfFrameWriter(const MmfFrameWriter&) = delete;
    MmfFrameWriter& operator=(const MmfFrameWriter&) = delete;

    bool init(const wchar_t* mmf_name, uint32_t width, uint32_t height,
              uint32_t slot_count = 3u) {
        shutdown();
        if (mmf_name == nullptr || *mmf_name == L'\0' || width == 0 ||
            height == 0 ||
            slot_count < SAO_UI_SOPF_MMF_V1_MIN_SLOT_COUNT ||
            slot_count > SAO_UI_SOPF_MMF_MAX_SLOT_COUNT) {
            return false;
        }
        const uint64_t pixel_bytes = static_cast<uint64_t>(width) *
                                     static_cast<uint64_t>(height) * 4ull;
        const uint64_t slot_stride = ((pixel_bytes + 4095ull) / 4096ull) * 4096ull;
        const uint64_t total =
            static_cast<uint64_t>(SAO_UI_SOPF_MMF_HEADER_BYTES) +
            static_cast<uint64_t>(slot_count) * slot_stride;
        if (total > SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES) {
            return false;
        }
        mmf_handle_ = ::CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            static_cast<DWORD>((total >> 32) & 0xFFFFFFFFu),
            static_cast<DWORD>(total & 0xFFFFFFFFu), mmf_name);
        if (mmf_handle_ == nullptr) {
            return false;
        }
        view_ = static_cast<uint8_t*>(::MapViewOfFile(
            mmf_handle_, FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(total)));
        if (view_ == nullptr) {
            ::CloseHandle(mmf_handle_);
            mmf_handle_ = nullptr;
            return false;
        }
        header_ = reinterpret_cast<SaoUiSopfMmfHeaderV1*>(view_);
        slots_ = view_ + SAO_UI_SOPF_MMF_HEADER_BYTES;
        width_ = width;
        height_ = height;
        slot_count_ = slot_count;
        slot_stride_ = static_cast<uint32_t>(slot_stride);
        write_slot_ = 0u;
        write_seq_ = 0u;
        ::memset(view_, 0, static_cast<size_t>(total));
        header_->magic = SAO_UI_SOPF_MMF_MAGIC;
        header_->version = SAO_UI_SOPF_MMF_VERSION_V1;
        header_->frame_width = width;
        header_->frame_height = height;
        header_->slot_count = slot_count;
        header_->slot_stride = slot_stride_;
        header_->published_generation = 0ull;
        header_->published_slot = 0u;
        return true;
    }

    void shutdown() {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mmf_handle_ != nullptr) {
            ::CloseHandle(mmf_handle_);
            mmf_handle_ = nullptr;
        }
        header_ = nullptr;
        slots_ = nullptr;
        width_ = 0u;
        height_ = 0u;
        slot_count_ = 0u;
        slot_stride_ = 0u;
        write_slot_ = 0u;
        write_seq_ = 0u;
    }

    bool is_initialized() const { return view_ != nullptr; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t slot_stride() const { return slot_stride_; }
    uint64_t last_committed_generation() const { return write_seq_; }

    // Return the byte address of the current write slot's payload.  Caller
    // writes width*height BGRA (premultiplied alpha) into it, then calls
    // commit_frame().  Only one begin/commit pair may be in flight per
    // writer instance.
    uint8_t* current_slot_pixels() {
        return slots_ + static_cast<size_t>(write_slot_) * slot_stride_;
    }
    size_t current_slot_bytes() const {
        return static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u;
    }

    // Complete the slot that current_slot_pixels() points at.  SOPF V1
    // publishes the next write slot; readers consume its predecessor.
    void commit_frame() {
        if (header_ == nullptr) {
            return;
        }
        write_seq_ += 1ull;
        const uint32_t next_write_slot = (write_slot_ + 1u) % slot_count_;
        std::atomic_ref<uint32_t> published_slot_ref(header_->published_slot);
        std::atomic_ref<uint64_t> published_gen_ref(
            header_->published_generation);
        // Slot pointer must land before generation bumps -- readers that see
        // the new generation must be able to compute (slot + N - 1) % N and
        // land on the freshly written payload.
        published_slot_ref.store(next_write_slot, std::memory_order_release);
        published_gen_ref.store(write_seq_, std::memory_order_release);
        write_slot_ = next_write_slot;
    }

    bool restore_frame(const uint8_t* pixels, size_t bytes,
                       uint64_t generation) {
        if (header_ == nullptr || pixels == nullptr ||
            bytes != current_slot_bytes()) {
            return false;
        }
        ::memcpy(current_slot_pixels(), pixels, bytes);
        write_seq_ = generation;
        const uint32_t next_write_slot = (write_slot_ + 1u) % slot_count_;
        std::atomic_ref<uint32_t> published_slot_ref(header_->published_slot);
        std::atomic_ref<uint64_t> published_gen_ref(
            header_->published_generation);
        published_slot_ref.store(next_write_slot, std::memory_order_release);
        published_gen_ref.store(generation, std::memory_order_release);
        write_slot_ = next_write_slot;
        return true;
    }

    // Destroy the MMF and re-create with new dimensions.  Consumers
    // (compositor) re-open the MMF every tick and will pick up the new
    // header on the next poll.  Layer geometry mismatch will be detected
    // by compositor validate_mmf_header_locked() and the consumer must
    // re-create its layer with matching size before re-attaching.
    bool resize(const wchar_t* mmf_name, uint32_t new_width,
                uint32_t new_height) {
        return init(mmf_name, new_width, new_height, slot_count_ == 0u
                                                          ? 3u
                                                          : slot_count_);
    }

private:
    HANDLE mmf_handle_ = nullptr;
    uint8_t* view_ = nullptr;
    SaoUiSopfMmfHeaderV1* header_ = nullptr;
    uint8_t* slots_ = nullptr;
    uint32_t width_ = 0u;
    uint32_t height_ = 0u;
    uint32_t slot_count_ = 0u;
    uint32_t slot_stride_ = 0u;
    uint32_t write_slot_ = 0u;
    uint64_t write_seq_ = 0u;
};

}  // namespace sao::ai_editor

#endif  // _WIN32
