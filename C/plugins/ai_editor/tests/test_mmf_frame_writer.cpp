// Catch2 tests for MmfFrameWriter (mmf_frame_writer.h).
//
// Covers:
//   * init writes correct SOPF V1 header (magic/version/w/h/slot_count/stride)
//   * commit_frame bumps generation monotonically + rotates slot correctly
//   * reader (OpenFileMappingW in same process) sees identical header +
//     payload
//   * generation stays stable when begin/commit is skipped (idle path)
//   * resize destroys and re-creates MMF with new dimensions
//   * init rejects invalid inputs (null name, zero dims, out-of-range slot
//     count)

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "mmf_frame_writer.h"
#include "sao/ui/compositor.h"

namespace {

std::wstring unique_mmf_name(const wchar_t* prefix) {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    wchar_t buf[128]{};
    ::swprintf_s(buf, L"Local\\%ls_%08lX_%08lX", prefix,
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long>(counter.LowPart ^ counter.HighPart));
    return buf;
}

struct ReaderView {
    HANDLE handle = nullptr;
    uint8_t* view = nullptr;
    ~ReaderView() {
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
        }
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
    }
    bool open(const std::wstring& name, size_t total_bytes) {
        handle = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (handle == nullptr) {
            return false;
        }
        view = static_cast<uint8_t*>(
            ::MapViewOfFile(handle, FILE_MAP_READ, 0, 0, total_bytes));
        return view != nullptr;
    }
    const SaoUiSopfMmfHeaderV1* header() const {
        return reinterpret_cast<const SaoUiSopfMmfHeaderV1*>(view);
    }
    const uint8_t* slot_bytes(uint32_t slot) const {
        const auto* h = header();
        return view + SAO_UI_SOPF_MMF_HEADER_BYTES + slot * h->slot_stride;
    }
};

}  // namespace

TEST_CASE("MmfFrameWriter init writes SOPF V1 header",
          "[ai_editor][mmf]") {
    sao::ai_editor::MmfFrameWriter w;
    const auto name = unique_mmf_name(L"MmfWriterInitTest");
    REQUIRE(w.init(name.c_str(), 32u, 24u, 3u));
    REQUIRE(w.is_initialized());
    REQUIRE(w.width() == 32u);
    REQUIRE(w.height() == 24u);
    REQUIRE(w.slot_stride() == 4096u);  // 32*24*4=3072 → page-aligned 4096

    ReaderView r;
    const size_t total =
        SAO_UI_SOPF_MMF_HEADER_BYTES + 3u * static_cast<size_t>(w.slot_stride());
    REQUIRE(r.open(name, total));
    const auto* h = r.header();
    REQUIRE(h->magic == SAO_UI_SOPF_MMF_MAGIC);
    REQUIRE(h->version == SAO_UI_SOPF_MMF_VERSION_V1);
    REQUIRE(h->frame_width == 32u);
    REQUIRE(h->frame_height == 24u);
    REQUIRE(h->slot_count == 3u);
    REQUIRE(h->slot_stride == 4096u);
    REQUIRE(h->published_generation == 0ull);
    REQUIRE(h->published_slot == 0u);
}

TEST_CASE("MmfFrameWriter commit_frame bumps generation and rotates slot",
          "[ai_editor][mmf]") {
    sao::ai_editor::MmfFrameWriter w;
    const auto name = unique_mmf_name(L"MmfWriterCommitTest");
    REQUIRE(w.init(name.c_str(), 16u, 16u, 3u));

    ReaderView r;
    const size_t total = SAO_UI_SOPF_MMF_HEADER_BYTES + 3u * 4096u;
    REQUIRE(r.open(name, total));

    for (uint64_t gen = 1u; gen <= 5u; ++gen) {
        uint8_t* pixels = w.current_slot_pixels();
        ::memset(pixels, static_cast<int>(gen & 0xFFu), w.current_slot_bytes());
        w.commit_frame();

        std::atomic_ref<uint64_t> published_gen(
            const_cast<uint64_t&>(r.header()->published_generation));
        std::atomic_ref<uint32_t> published_slot(
            const_cast<uint32_t&>(r.header()->published_slot));
        REQUIRE(published_gen.load(std::memory_order_acquire) == gen);
        // Writer publishes the just-written slot index; ring advances 0,1,2,0,1,...
        const uint32_t expected_slot = static_cast<uint32_t>((gen - 1u) % 3u);
        REQUIRE(published_slot.load(std::memory_order_acquire) == expected_slot);
    }
}

TEST_CASE("MmfFrameWriter idle skips do not bump generation",
          "[ai_editor][mmf]") {
    sao::ai_editor::MmfFrameWriter w;
    const auto name = unique_mmf_name(L"MmfWriterIdleTest");
    REQUIRE(w.init(name.c_str(), 8u, 8u, 3u));

    ReaderView r;
    const size_t total = SAO_UI_SOPF_MMF_HEADER_BYTES + 3u * 4096u;
    REQUIRE(r.open(name, total));

    REQUIRE(r.header()->published_generation == 0ull);
    // Simulate 100 idle ticks without calling commit_frame.
    for (int i = 0; i < 100; ++i) {
        // no-op: writer skips the whole begin/commit pair
    }
    REQUIRE(r.header()->published_generation == 0ull);

    // One real commit.
    ::memset(w.current_slot_pixels(), 0x7Fu, w.current_slot_bytes());
    w.commit_frame();
    REQUIRE(r.header()->published_generation == 1ull);
}

TEST_CASE("MmfFrameWriter reader sees payload written by writer",
          "[ai_editor][mmf]") {
    sao::ai_editor::MmfFrameWriter w;
    const auto name = unique_mmf_name(L"MmfWriterPayloadTest");
    REQUIRE(w.init(name.c_str(), 4u, 4u, 3u));

    ReaderView r;
    const size_t total = SAO_UI_SOPF_MMF_HEADER_BYTES + 3u * 4096u;
    REQUIRE(r.open(name, total));

    uint8_t marker[64]{};
    for (size_t i = 0; i < 64; ++i) {
        marker[i] = static_cast<uint8_t>(0xA0u + (i & 0x3Fu));
    }
    ::memcpy(w.current_slot_pixels(), marker, sizeof(marker));
    w.commit_frame();

    const uint32_t published_slot = r.header()->published_slot;
    const uint8_t* slot_data = r.slot_bytes(published_slot);
    REQUIRE(::memcmp(slot_data, marker, sizeof(marker)) == 0);
}

TEST_CASE("MmfFrameWriter resize destroys and re-creates with new size",
          "[ai_editor][mmf]") {
    sao::ai_editor::MmfFrameWriter w;
    const auto name = unique_mmf_name(L"MmfWriterResizeTest");
    REQUIRE(w.init(name.c_str(), 16u, 16u, 3u));
    REQUIRE(w.width() == 16u);
    REQUIRE(w.height() == 16u);

    REQUIRE(w.resize(name.c_str(), 64u, 48u));
    REQUIRE(w.width() == 64u);
    REQUIRE(w.height() == 48u);

    const size_t new_total =
        SAO_UI_SOPF_MMF_HEADER_BYTES + 3u * static_cast<size_t>(w.slot_stride());
    ReaderView r;
    REQUIRE(r.open(name, new_total));
    REQUIRE(r.header()->frame_width == 64u);
    REQUIRE(r.header()->frame_height == 48u);
    REQUIRE(r.header()->published_generation == 0ull);
}

TEST_CASE("MmfFrameWriter init rejects invalid inputs",
          "[ai_editor][mmf]") {
    sao::ai_editor::MmfFrameWriter w;
    REQUIRE_FALSE(w.init(nullptr, 16u, 16u, 3u));
    REQUIRE_FALSE(w.init(L"", 16u, 16u, 3u));
    REQUIRE_FALSE(w.init(L"Local\\bad", 0u, 16u, 3u));
    REQUIRE_FALSE(w.init(L"Local\\bad", 16u, 0u, 3u));
    REQUIRE_FALSE(w.init(L"Local\\bad", 16u, 16u, 0u));
    REQUIRE_FALSE(
        w.init(L"Local\\bad", 16u, 16u, SAO_UI_SOPF_MMF_MAX_SLOT_COUNT + 1u));
    REQUIRE_FALSE(w.is_initialized());
}
