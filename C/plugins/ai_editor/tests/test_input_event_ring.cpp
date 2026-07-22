// Catch2 tests for InputEventRingWriter / InputEventRingReader.
//
// Covers header protocol invariants, single-slot round-trip, batch drain,
// overflow detection, and rejection of malformed shared sections.

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <string>
#include <thread>

#include "input_event_ring.h"

namespace {

std::wstring unique_ring_name(const wchar_t* prefix) {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    wchar_t buf[128]{};
    ::swprintf_s(buf, L"Local\\%ls_%08lX_%08lX", prefix,
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long>(counter.LowPart ^ counter.HighPart));
    return buf;
}

sao::ai_editor::InputEvent make_key_down(uint32_t vkey, uint32_t modifiers) {
    sao::ai_editor::InputEvent ev{};
    ev.type = sao::ai_editor::INPUT_EVENT_KEY_DOWN;
    ev.modifiers = modifiers;
    ev.code = vkey;
    return ev;
}

sao::ai_editor::InputEvent make_mouse_move(int32_t x, int32_t y) {
    sao::ai_editor::InputEvent ev{};
    ev.type = sao::ai_editor::INPUT_EVENT_MOUSE_MOVE;
    ev.x = x;
    ev.y = y;
    return ev;
}

}  // namespace

TEST_CASE("InputEventRing writer + reader round trip",
          "[ai_editor][input_ring]") {
    const auto name = unique_ring_name(L"InputRingRoundTrip");
    sao::ai_editor::InputEventRingWriter w;
    REQUIRE(w.init(name.c_str(), 64u));
    REQUIRE(w.is_initialized());

    sao::ai_editor::InputEventRingReader r;
    REQUIRE(r.open(name.c_str()));

    REQUIRE(w.push(make_key_down('A', sao::ai_editor::INPUT_MOD_SHIFT)));
    REQUIRE(w.push(make_mouse_move(120, 240)));

    std::array<sao::ai_editor::InputEvent, 8> out{};
    uint32_t overflow = 999u;
    const uint32_t got =
        r.try_read_batch(out.data(), 8u, overflow);
    REQUIRE(got == 2u);
    REQUIRE(overflow == 0u);
    REQUIRE(out[0].type == sao::ai_editor::INPUT_EVENT_KEY_DOWN);
    REQUIRE(out[0].code == 'A');
    REQUIRE(out[0].modifiers == sao::ai_editor::INPUT_MOD_SHIFT);
    REQUIRE(out[0].sequence == 0u);
    REQUIRE(out[1].type == sao::ai_editor::INPUT_EVENT_MOUSE_MOVE);
    REQUIRE(out[1].x == 120);
    REQUIRE(out[1].y == 240);
    REQUIRE(out[1].sequence == 1u);

    // Second drain sees nothing new.
    overflow = 999u;
    REQUIRE(r.try_read_batch(out.data(), 8u, overflow) == 0u);
    REQUIRE(overflow == 0u);
}

TEST_CASE("InputEventRing overflow drops oldest events",
          "[ai_editor][input_ring]") {
    const auto name = unique_ring_name(L"InputRingOverflow");
    sao::ai_editor::InputEventRingWriter w;
    REQUIRE(w.init(name.c_str(), 8u));  // small ring to force overflow

    sao::ai_editor::InputEventRingReader r;
    REQUIRE(r.open(name.c_str()));

    // Push 20 events into an 8-slot ring without reading.
    for (uint32_t i = 0; i < 20u; ++i) {
        auto ev = make_key_down('A' + (i % 26u), 0u);
        ev.x = static_cast<int32_t>(i);
        REQUIRE(w.push(ev));
    }

    std::array<sao::ai_editor::InputEvent, 16> out{};
    uint32_t overflow = 0u;
    const uint32_t got =
        r.try_read_batch(out.data(), out.size(), overflow);
    REQUIRE(got == 8u);
    REQUIRE(overflow == 12u);  // 20 written - 8 buffer - 0 read = 12 lost
    // First surviving event carries writer sequence 12, holds 'A'+12%26.
    REQUIRE(out[0].sequence == 12u);
    REQUIRE(out[0].code == static_cast<uint32_t>('A' + 12u));
    REQUIRE(out[0].x == 12);
    REQUIRE(out[7].sequence == 19u);
    REQUIRE(out[7].x == 19);
}

TEST_CASE("InputEventRing init rejects invalid parameters",
          "[ai_editor][input_ring]") {
    sao::ai_editor::InputEventRingWriter w;
    REQUIRE_FALSE(w.init(nullptr, 64u));
    REQUIRE_FALSE(w.init(L"", 64u));
    REQUIRE_FALSE(w.init(L"Local\\bad", 0u));
    REQUIRE_FALSE(w.init(L"Local\\bad", 3u));  // not power of two
    REQUIRE_FALSE(w.init(L"Local\\bad", 100u));  // not power of two
    REQUIRE_FALSE(
        w.init(L"Local\\bad", sao::ai_editor::kInputRingMaxSlots * 2u));
    REQUIRE_FALSE(w.is_initialized());
}

TEST_CASE("InputEventRing reader rejects wrong magic",
          "[ai_editor][input_ring]") {
    const auto name = unique_ring_name(L"InputRingBadMagic");
    // Create a shared section but do not initialize header.
    const size_t total =
        sao::ai_editor::kInputRingHeaderBytes +
        static_cast<size_t>(64u) * sao::ai_editor::kInputEventBytes;
    HANDLE h = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                     PAGE_READWRITE, 0,
                                     static_cast<DWORD>(total), name.c_str());
    REQUIRE(h != nullptr);
    void* view = ::MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, total);
    REQUIRE(view != nullptr);
    ::memset(view, 0xAB, total);  // garbage magic

    sao::ai_editor::InputEventRingReader r;
    REQUIRE_FALSE(r.open(name.c_str()));

    ::UnmapViewOfFile(view);
    ::CloseHandle(h);
}

TEST_CASE("InputEventRing empty ring returns zero without side effects",
          "[ai_editor][input_ring]") {
    const auto name = unique_ring_name(L"InputRingEmpty");
    sao::ai_editor::InputEventRingWriter w;
    REQUIRE(w.init(name.c_str(), 32u));

    sao::ai_editor::InputEventRingReader r;
    REQUIRE(r.open(name.c_str()));

    std::array<sao::ai_editor::InputEvent, 4> out{};
    uint32_t overflow = 999u;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(r.try_read_batch(out.data(), 4u, overflow) == 0u);
        REQUIRE(overflow == 0u);
    }
    REQUIRE(r.read_sequence() == 0u);
}
