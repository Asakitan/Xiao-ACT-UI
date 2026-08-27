#pragma once

// Shared-memory SPSC input event ring.  Main-process compositor is the
// producer (mouse / keyboard / IME captured by overlay_host WndProc,
// routed to the focused layer's ring), subprocess is the consumer
// (dispatches into its self-render UI).
//
// Layout (single MMF section named by handshake):
//   [64 B InputRingHeader]
//   [event_count * sizeof(InputEvent)]  each event slot is 64 B fixed.
//
// SPSC lockfree contract:
//   * Producer bumps `write_seq` after storing the slot payload.
//   * Consumer reads slots [read_seq, write_seq), then bumps `read_seq`.
//   * event_count must be a power of two; slot index = seq & (count-1).
//   * Ring is drop-tolerant: when write_seq - read_seq >= event_count
//     the producer overwrites the oldest unread event.  Consumer must
//     detect the overflow (see try_read_batch's returned dropped count)
//     and recover (in practice: refresh full focus state, e.g. request
//     a re-sync from the producer).
//
// Optional Named Event (`<name>_evt`, auto-reset) can be created by the
// consumer for blocking wait; producer sets it after every write.
// Polling-only usage also works and is what the current spike uses.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sao::ai_editor {

constexpr uint32_t kInputRingMagic = 0x5645494Eu;  // 'NIEV'
constexpr uint32_t kInputRingVersion = 1u;
constexpr uint32_t kInputRingHeaderBytes = 64u;
constexpr uint32_t kInputEventBytes = 64u;
constexpr uint32_t kInputRingDefaultSlots = 64u;
constexpr uint32_t kInputRingMaxSlots = 4096u;

enum InputEventType : uint32_t {
    INPUT_EVENT_NONE = 0u,
    INPUT_EVENT_MOUSE_MOVE = 1u,
    INPUT_EVENT_MOUSE_BUTTON = 2u,
    INPUT_EVENT_MOUSE_WHEEL = 3u,
    INPUT_EVENT_KEY_DOWN = 4u,
    INPUT_EVENT_KEY_UP = 5u,
    INPUT_EVENT_CHAR = 6u,
    INPUT_EVENT_IME_COMPOSITION = 7u,
    INPUT_EVENT_IME_COMMIT = 8u,
    INPUT_EVENT_RESIZE = 9u,
    INPUT_EVENT_FOCUS = 10u,
    INPUT_EVENT_CLOSE = 11u,
};

// Modifier bit flags (mirror Windows VK modifier semantics).
constexpr uint32_t INPUT_MOD_SHIFT = 1u << 0;
constexpr uint32_t INPUT_MOD_CTRL = 1u << 1;
constexpr uint32_t INPUT_MOD_ALT = 1u << 2;
constexpr uint32_t INPUT_MOD_WIN = 1u << 3;
constexpr uint32_t INPUT_MOD_CAPS = 1u << 4;

// Fixed 64-byte layout across all event types.  Reader picks fields
// based on `type`.  Unused fields are zero.
struct InputEvent {
    uint32_t type;
    uint32_t modifiers;
    int32_t x;               // mouse x (layer-local, unscaled) / resize new_w
    int32_t y;               // mouse y / resize new_h
    uint32_t code;           // vkey / scancode / unicode codepoint
    uint32_t button;         // mouse button flag (VK_LBUTTON etc.)
    int32_t wheel_delta;     // WHEEL_DELTA units
    uint32_t sequence;       // producer-side sequence for debugging
    // For IME_COMPOSITION / IME_COMMIT, reserved[0] is an optional UTF-16
    // code-unit count (0..14), and reserved[1..7] pack two UTF-16 units per
    // word, low unit first. When the count is zero, `code` carries one
    // Unicode scalar value; zero means an empty composition update.
    uint32_t reserved[8];
};
static_assert(sizeof(InputEvent) == kInputEventBytes, "InputEvent must be 64 bytes");

inline void copy_input_event_payload(InputEvent& destination,
                                     const InputEvent& source) noexcept {
    constexpr size_t marker_offset = offsetof(InputEvent, sequence);
    ::memcpy(&destination, &source, marker_offset);
    ::memcpy(reinterpret_cast<uint8_t*>(&destination) + marker_offset +
                 sizeof(source.sequence),
             reinterpret_cast<const uint8_t*>(&source) + marker_offset +
                 sizeof(source.sequence),
             sizeof(InputEvent) - marker_offset - sizeof(source.sequence));
}

struct InputRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t event_count;    // power of two
    uint32_t event_size;     // == sizeof(InputEvent)
    uint64_t write_seq;      // producer atomic release
    uint64_t read_seq;       // consumer atomic release
    uint8_t reserved[32];
};
static_assert(sizeof(InputRingHeader) == kInputRingHeaderBytes,
              "InputRingHeader must be 64 bytes");

namespace detail {

inline bool is_power_of_two(uint32_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

inline size_t total_bytes(uint32_t event_count) {
    return static_cast<size_t>(kInputRingHeaderBytes) +
           static_cast<size_t>(event_count) *
               static_cast<size_t>(kInputEventBytes);
}

}  // namespace detail

// Producer (main process, compositor input capture side).
class InputEventRingWriter {
public:
    InputEventRingWriter() = default;
    ~InputEventRingWriter() { shutdown(); }

    InputEventRingWriter(const InputEventRingWriter&) = delete;
    InputEventRingWriter& operator=(const InputEventRingWriter&) = delete;

    bool init(const wchar_t* name,
              uint32_t event_count = kInputRingDefaultSlots) {
        shutdown();
        if (name == nullptr || *name == L'\0' ||
            !detail::is_power_of_two(event_count) ||
            event_count > kInputRingMaxSlots) {
            return false;
        }
        const size_t total = detail::total_bytes(event_count);
        handle_ = ::CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(total), name);
        if (handle_ == nullptr) {
            return false;
        }
        if (::GetLastError() == ERROR_ALREADY_EXISTS) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        view_ = static_cast<uint8_t*>(::MapViewOfFile(
            handle_, FILE_MAP_WRITE, 0, 0, total));
        if (view_ == nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        ::memset(view_, 0, total);
        header_ = reinterpret_cast<InputRingHeader*>(view_);
        events_ = reinterpret_cast<InputEvent*>(view_ + kInputRingHeaderBytes);
        header_->magic = kInputRingMagic;
        header_->version = kInputRingVersion;
        header_->event_count = event_count;
        header_->event_size = kInputEventBytes;
        header_->reserved[0] = 1u;
        event_count_ = event_count;
        seq_ = 0u;
        return true;
    }

    void shutdown() {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
        header_ = nullptr;
        events_ = nullptr;
        event_count_ = 0u;
        seq_ = 0u;
    }

    bool is_initialized() const { return view_ != nullptr; }

    bool push(const InputEvent& ev) {
        if (header_ == nullptr) {
            return false;
        }
        const uint32_t slot = static_cast<uint32_t>(seq_ & (event_count_ - 1u));
        const uint32_t stable_sequence =
            static_cast<uint32_t>((seq_ << 1u) + 2u);
        const uint32_t writing_sequence = stable_sequence - 1u;
        std::atomic_ref<uint32_t> slot_sequence(events_[slot].sequence);
        slot_sequence.store(writing_sequence, std::memory_order_release);
        copy_input_event_payload(events_[slot], ev);
        slot_sequence.store(stable_sequence, std::memory_order_release);
        std::atomic_ref<uint64_t> write_seq_ref(header_->write_seq);
        seq_ += 1ull;
        write_seq_ref.store(seq_, std::memory_order_release);
        return true;
    }

    uint64_t sequence() const { return seq_; }

private:
    HANDLE handle_ = nullptr;
    uint8_t* view_ = nullptr;
    InputRingHeader* header_ = nullptr;
    InputEvent* events_ = nullptr;
    uint32_t event_count_ = 0u;
    uint64_t seq_ = 0u;
};

// Consumer (subprocess UI thread).
class InputEventRingReader {
public:
    InputEventRingReader() = default;
    ~InputEventRingReader() { shutdown(); }

    InputEventRingReader(const InputEventRingReader&) = delete;
    InputEventRingReader& operator=(const InputEventRingReader&) = delete;

    bool open(const wchar_t* name) {
        shutdown();
        if (name == nullptr || *name == L'\0') {
            return false;
        }
        handle_ = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (handle_ == nullptr) {
            return false;
        }
        // Two-step map: header then full ring after event_count is known.
        uint8_t* header_only = static_cast<uint8_t*>(::MapViewOfFile(
            handle_, FILE_MAP_READ, 0, 0, kInputRingHeaderBytes));
        if (header_only == nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        const InputRingHeader hdr =
            *reinterpret_cast<const InputRingHeader*>(header_only);
        ::UnmapViewOfFile(header_only);
        if (hdr.magic != kInputRingMagic ||
            hdr.version != kInputRingVersion ||
            hdr.event_size != kInputEventBytes || hdr.reserved[0] != 1u ||
            !detail::is_power_of_two(hdr.event_count) ||
            hdr.event_count > kInputRingMaxSlots) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        const size_t total = detail::total_bytes(hdr.event_count);
        view_ = static_cast<uint8_t*>(::MapViewOfFile(
            handle_, FILE_MAP_ALL_ACCESS, 0, 0, total));
        if (view_ == nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        header_ = reinterpret_cast<InputRingHeader*>(view_);
        events_ = reinterpret_cast<const InputEvent*>(
            view_ + kInputRingHeaderBytes);
        event_count_ = hdr.event_count;
        std::atomic_ref<uint64_t> write_seq_ref(header_->write_seq);
        const uint64_t write_seq =
            write_seq_ref.load(std::memory_order_acquire);
        std::atomic_ref<uint64_t> read_seq_ref(header_->read_seq);
        read_seq_ = read_seq_ref.load(std::memory_order_acquire);
        if (read_seq_ > write_seq) {
            read_seq_ = write_seq;
            read_seq_ref.store(read_seq_, std::memory_order_release);
        }
        return true;
    }

    void shutdown() {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
        header_ = nullptr;
        events_ = nullptr;
        event_count_ = 0u;
        read_seq_ = 0u;
    }

    bool is_open() const { return view_ != nullptr; }

    // Drain up to `max_events` new events into `out`.  Returns the count
    // read; `overflow_dropped` receives the number of events lost since
    // the last drain (write - read > event_count).  Overflow means the
    // producer overwrote unread slots -- caller should treat this as a
    // signal to re-sync focus state.
    uint32_t try_read_batch(InputEvent* out, uint32_t max_events,
                            uint32_t& overflow_dropped) {
        overflow_dropped = 0u;
        if (header_ == nullptr || out == nullptr || max_events == 0u) {
            return 0u;
        }
        std::atomic_ref<uint64_t> write_seq_ref(header_->write_seq);
        const uint64_t write_seq =
            write_seq_ref.load(std::memory_order_acquire);
        if (write_seq <= read_seq_) {
            return 0u;
        }
        const uint64_t available = write_seq - read_seq_;
        uint64_t start = read_seq_;
        if (available > event_count_) {
            overflow_dropped =
                static_cast<uint32_t>(available - event_count_);
            start = write_seq - event_count_;
        }
        const uint64_t end = write_seq;
        uint32_t count = 0u;
        for (uint64_t s = start; s < end && count < max_events; ++s) {
            const uint32_t slot =
                static_cast<uint32_t>(s & (event_count_ - 1u));
            std::atomic_ref<uint32_t> slot_sequence(const_cast<uint32_t&>(events_[slot].sequence));
            InputEvent local{};
            bool stable = false;
            for (uint32_t attempt = 0u; attempt < 8u; ++attempt) {
                const uint32_t before =
                    slot_sequence.load(std::memory_order_acquire);
                if ((before & 1u) != 0u) {
                    continue;
                }
                copy_input_event_payload(local, events_[slot]);
                const uint32_t after =
                    slot_sequence.load(std::memory_order_acquire);
                if (before == after && (after & 1u) == 0u) {
                    local.sequence = after;
                    stable = true;
                    break;
                }
            }
            if (!stable) {
                break;
            }
            out[count++] = local;
        }
        read_seq_ = start + count;
        std::atomic_ref<uint64_t> read_seq_ref(header_->read_seq);
        read_seq_ref.store(read_seq_, std::memory_order_release);
        return count;
    }

    uint64_t read_sequence() const { return read_seq_; }

private:
    HANDLE handle_ = nullptr;
    uint8_t* view_ = nullptr;
    InputRingHeader* header_ = nullptr;
    const InputEvent* events_ = nullptr;
    uint32_t event_count_ = 0u;
    uint64_t read_seq_ = 0u;
};

}  // namespace sao::ai_editor

#endif  // _WIN32
