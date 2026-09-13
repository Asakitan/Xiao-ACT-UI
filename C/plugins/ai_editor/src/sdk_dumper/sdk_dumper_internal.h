#pragma once

#include "../native_utils.h"
#include "sao/core/memory.h"
#include "sao/core/process.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::ai_editor::sdk_dumper::detail {

namespace fs = std::filesystem;

constexpr uint64_t kProcessReadBudget = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMinimumUserAddress = 0x10000ull;
constexpr uint64_t kMaximumUserAddress = 0x00007fffffffffffull;
constexpr size_t kMaximumOutputBytes = 32ull * 1024ull * 1024ull;

inline bool checked_add(uint64_t left, uint64_t right, uint64_t& result) noexcept {
    if (left > (std::numeric_limits<uint64_t>::max)() - right) {
        return false;
    }
    result = left + right;
    return true;
}

inline bool checked_sub(uint64_t left, uint64_t right, uint64_t& result) noexcept {
    if (left < right) {
        return false;
    }
    result = left - right;
    return true;
}

inline bool checked_mul(uint64_t left, uint64_t right, uint64_t& result) noexcept {
    if (left != 0 && right > (std::numeric_limits<uint64_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

inline bool checked_add_signed(uint64_t value, int32_t displacement, uint64_t& result) noexcept {
    if (displacement >= 0) {
        return checked_add(value, static_cast<uint32_t>(displacement), result);
    }
    const uint64_t magnitude = static_cast<uint64_t>(-(static_cast<int64_t>(displacement)));
    return checked_sub(value, magnitude, result);
}

inline bool valid_user_range(uint64_t address, size_t length) noexcept {
    if (address < kMinimumUserAddress || address > kMaximumUserAddress) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    const uint64_t tail = static_cast<uint64_t>(length - 1);
    return tail <= kMaximumUserAddress - address;
}

inline bool valid_pointer_target(uint64_t value, uint8_t pointer_size) noexcept {
    if (pointer_size != 4 && pointer_size != 8) {
        return false;
    }
    if (pointer_size == 4 && value > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    return value >= kMinimumUserAddress && value <= kMaximumUserAddress;
}

inline bool valid_pointer(uint64_t value, uint8_t pointer_size) noexcept {
    return valid_pointer_target(value, pointer_size) &&
           (value & (pointer_size - 1u)) == 0;
}

class Deadline final {
public:
    explicit Deadline(uint32_t timeout_ms)
        : value_(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)) {}

    bool expired() const noexcept { return std::chrono::steady_clock::now() >= value_; }

private:
    std::chrono::steady_clock::time_point value_;
};

enum class StopReason { None, Timeout, Budget };

class ReadBudget final {
public:
    ReadBudget(sao_core_process_handle_t process, const Deadline& deadline)
        : process_(process), deadline_(deadline) {}

    bool checkpoint() noexcept {
        if (reason_ != StopReason::None) {
            return false;
        }
        if (deadline_.expired()) {
            reason_ = StopReason::Timeout;
            return false;
        }
        return true;
    }

    bool read(uint64_t address, void* output, size_t length) noexcept {
        if (!checkpoint() || (length != 0 && output == nullptr) ||
            !valid_user_range(address, length)) {
            return false;
        }
        if (static_cast<uint64_t>(length) > kProcessReadBudget - bytes_used_) {
            reason_ = StopReason::Budget;
            return false;
        }
        bytes_used_ += static_cast<uint64_t>(length);
        size_t bytes_read = 0;
        const sao_status_t status =
            sao_core_mem_read(process_, address, output, length, &bytes_read);
        if (deadline_.expired()) {
            reason_ = StopReason::Timeout;
            return false;
        }
        return status == SAO_STATUS_OK && bytes_read == length;
    }

    template <typename Value>
    bool read_value(uint64_t address, Value& output) noexcept {
        output = {};
        return read(address, &output, sizeof(output));
    }

    StopReason reason() const noexcept { return reason_; }
    uint64_t bytes_used() const noexcept { return bytes_used_; }
    uint64_t bytes_remaining() const noexcept { return kProcessReadBudget - bytes_used_; }

private:
    sao_core_process_handle_t process_ = nullptr;
    const Deadline& deadline_;
    uint64_t bytes_used_ = 0;
    StopReason reason_ = StopReason::None;
};

inline bool set_stop_error(const ReadBudget& reader, std::string& error) {
    if (reader.reason() == StopReason::Timeout) {
        error = "timeout: monotonic dump deadline expired";
        return true;
    }
    if (reader.reason() == StopReason::Budget) {
        error = "budget: cumulative process reads exceeded 64 MiB";
        return true;
    }
    return false;
}

class ProcessHandle final {
public:
    ~ProcessHandle() {
        if (value_ != nullptr) {
            sao_core_process_close(value_);
        }
    }

    ProcessHandle() = default;
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    sao_status_t open(uint32_t pid) noexcept {
        return sao_core_process_open(pid, SAO_PROCESS_ACCESS_INFO | SAO_PROCESS_ACCESS_READ,
                                     &value_);
    }

    sao_core_process_handle_t get() const noexcept { return value_; }

private:
    sao_core_process_handle_t value_ = nullptr;
};

struct ModuleInfo final {
    std::string name;
    std::string lower_name;
    uint64_t base = 0;
    uint64_t size = 0;
};

inline std::string ascii_lower(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

inline std::string base_name_lower(std::string value) {
    const size_t slash = value.find_last_of("\\/");
    if (slash != std::string::npos) {
        value.erase(0, slash + 1);
    }
    return ascii_lower(std::move(value));
}

inline bool module_contains(const ModuleInfo& module, uint64_t address,
                            size_t length = 1) noexcept {
    if (module.size == 0 || !valid_user_range(module.base, 1) || address < module.base) {
        return false;
    }
    uint64_t module_end = 0;
    uint64_t address_end = 0;
    return checked_add(module.base, module.size, module_end) &&
           checked_add(address, static_cast<uint64_t>(length), address_end) &&
           address_end >= address && address_end <= module_end;
}

inline bool address_in_modules(const std::vector<ModuleInfo>& modules, uint64_t address,
                               size_t length = 1) noexcept {
    for (const ModuleInfo& module : modules) {
        if (module_contains(module, address, length)) {
            return true;
        }
    }
    return false;
}

inline bool enumerate_modules(sao_core_process_handle_t process,
                              std::vector<ModuleInfo>& modules,
                              std::string& error) {
    size_t entry_count = 0;
    size_t names_used = 0;
    sao_status_t status = sao_core_process_enum_modules(process, nullptr, 0, &entry_count, nullptr,
                                                         0, &names_used);
    if (status != SAO_STATUS_OK) {
        error = "capability: loaded module enumeration is unavailable";
        return false;
    }
    if (entry_count == 0 || entry_count > 1024 || names_used == 0 ||
        names_used > 1024 * 1024) {
        error = "capability: loaded module inventory exceeds bounded limits";
        return false;
    }

    std::vector<SaoModuleEntry> entries(entry_count);
    std::vector<char> names(names_used);
    size_t actual_count = 0;
    size_t actual_names = 0;
    status = sao_core_process_enum_modules(process, entries.data(), entries.size(), &actual_count,
                                           names.data(), names.size(), &actual_names);
    if (status != SAO_STATUS_OK || actual_count > entries.size() || actual_names > names.size()) {
        error = "capability: loaded module inventory changed during enumeration";
        return false;
    }

    modules.clear();
    modules.reserve(actual_count);
    for (size_t index = 0; index < actual_count; ++index) {
        const SaoModuleEntry& entry = entries[index];
        if (entry.name_offset >= actual_names || entry.module_size == 0) {
            error = "capability: loaded module entry is malformed";
            return false;
        }
        const char* raw_name = names.data() + entry.name_offset;
        const size_t remaining = actual_names - entry.name_offset;
        const void* terminator = std::memchr(raw_name, '\0', remaining);
        if (terminator == nullptr) {
            error = "capability: loaded module name is unterminated";
            return false;
        }
        const size_t name_length = static_cast<const char*>(terminator) - raw_name;
        if (name_length == 0 || name_length > 512) {
            error = "capability: loaded module name exceeds bounded limits";
            return false;
        }
        uint64_t module_end = 0;
        if (!valid_user_range(entry.base_address, 1) ||
            !checked_add(entry.base_address, entry.module_size, module_end) ||
            module_end <= entry.base_address || module_end - 1 > kMaximumUserAddress) {
            error = "capability: loaded module range is malformed";
            return false;
        }
        ModuleInfo module;
        module.name.assign(raw_name, name_length);
        module.lower_name = base_name_lower(module.name);
        module.base = entry.base_address;
        module.size = entry.module_size;
        modules.push_back(std::move(module));
    }
    std::sort(modules.begin(), modules.end(), [](const ModuleInfo& left, const ModuleInfo& right) {
        if (left.base != right.base) {
            return left.base < right.base;
        }
        return left.lower_name < right.lower_name;
    });
    return true;
}

struct PeSection final {
    std::string name;
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t characteristics = 0;
};

struct PeImage final {
    uint8_t pointer_size = 0;
    uint64_t size_of_image = 0;
    std::vector<PeSection> sections;
};

inline bool parse_pe_image(ReadBudget& reader, const ModuleInfo& module, PeImage& image,
                           std::string& error) {
    IMAGE_DOS_HEADER dos{};
    if (!module_contains(module, module.base, sizeof(dos)) ||
        !reader.read_value(module.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos.e_lfanew > 0x100000) {
        if (!set_stop_error(reader, error)) {
            error = "capability: module does not expose a bounded PE header";
        }
        return false;
    }

    uint64_t nt_address = 0;
    if (!checked_add(module.base, static_cast<uint32_t>(dos.e_lfanew), nt_address)) {
        error = "capability: PE header address overflow";
        return false;
    }
    DWORD signature = 0;
    IMAGE_FILE_HEADER file_header{};
    uint64_t file_header_address = 0;
    if (!checked_add(nt_address, sizeof(signature), file_header_address) ||
        !module_contains(module, nt_address, sizeof(signature) + sizeof(file_header))) {
        error = "capability: PE file header escapes the enumerated module";
        return false;
    }
    if (!reader.read_value(nt_address, signature) || signature != IMAGE_NT_SIGNATURE ||
        !reader.read_value(file_header_address, file_header) ||
        file_header.NumberOfSections == 0 || file_header.NumberOfSections > 96 ||
        file_header.SizeOfOptionalHeader < sizeof(uint16_t) ||
        file_header.SizeOfOptionalHeader > 4096) {
        if (!set_stop_error(reader, error)) {
            error = "capability: PE file header is malformed";
        }
        return false;
    }

    uint64_t optional_address = 0;
    if (!checked_add(file_header_address, sizeof(file_header), optional_address) ||
        !module_contains(module, optional_address, file_header.SizeOfOptionalHeader)) {
        error = "capability: PE optional header escapes the enumerated module";
        return false;
    }
    std::vector<uint8_t> optional(file_header.SizeOfOptionalHeader);
    if (!reader.read(optional_address, optional.data(), optional.size())) {
        if (!set_stop_error(reader, error)) {
            error = "capability: PE optional header is unreadable";
        }
        return false;
    }

    uint16_t magic = 0;
    std::memcpy(&magic, optional.data(), sizeof(magic));
    uint32_t size_of_image = 0;
    uint8_t pointer_size = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && file_header.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        optional.size() >= offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage) + sizeof(uint32_t)) {
        std::memcpy(&size_of_image,
                    optional.data() + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage),
                    sizeof(size_of_image));
        pointer_size = 8;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
               file_header.Machine == IMAGE_FILE_MACHINE_I386 &&
               optional.size() >= offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage) + sizeof(uint32_t)) {
        std::memcpy(&size_of_image,
                    optional.data() + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage),
                    sizeof(size_of_image));
        pointer_size = 4;
    } else {
        error = "capability: PE architecture or optional-header layout is unsupported";
        return false;
    }
    if (size_of_image == 0 || size_of_image > module.size) {
        error = "capability: PE image size is outside the enumerated module";
        return false;
    }
    uint64_t image_end = 0;
    uint64_t optional_end = 0;
    if (!checked_add(module.base, size_of_image, image_end) ||
        !checked_add(optional_address, optional.size(), optional_end) ||
        optional_end > image_end) {
        error = "capability: PE optional header escapes the image bounds";
        return false;
    }

    uint64_t section_address = 0;
    uint64_t section_bytes = 0;
    if (!checked_add(optional_address, file_header.SizeOfOptionalHeader, section_address) ||
        !checked_mul(file_header.NumberOfSections, sizeof(IMAGE_SECTION_HEADER), section_bytes) ||
        section_bytes > (std::numeric_limits<size_t>::max)()) {
        error = "capability: PE section-table bounds overflow";
        return false;
    }
    uint64_t section_end = 0;
    if (!checked_add(section_address, section_bytes, section_end) ||
        !module_contains(module, section_address, static_cast<size_t>(section_bytes)) ||
        section_end > image_end) {
        error = "capability: PE section table escapes the image bounds";
        return false;
    }
    std::vector<IMAGE_SECTION_HEADER> raw_sections(file_header.NumberOfSections);
    if (!reader.read(section_address, raw_sections.data(), static_cast<size_t>(section_bytes))) {
        if (!set_stop_error(reader, error)) {
            error = "capability: PE section table is unreadable";
        }
        return false;
    }

    image = {};
    image.pointer_size = pointer_size;
    image.size_of_image = size_of_image;
    image.sections.reserve(raw_sections.size());
    for (const IMAGE_SECTION_HEADER& raw : raw_sections) {
        const uint64_t rva = raw.VirtualAddress;
        uint64_t span = raw.Misc.VirtualSize != 0 ? raw.Misc.VirtualSize : raw.SizeOfRawData;
        if (span == 0 || rva >= size_of_image || span > size_of_image - rva ||
            rva >= module.size || span > module.size - rva) {
            continue;
        }
        PeSection section;
        const char* raw_name = reinterpret_cast<const char*>(raw.Name);
        size_t name_length = 0;
        while (name_length < IMAGE_SIZEOF_SHORT_NAME && raw_name[name_length] != '\0') {
            ++name_length;
        }
        section.name = ascii_lower(std::string(raw_name, name_length));
        if (!checked_add(module.base, rva, section.base)) {
            error = "capability: PE section address overflow";
            return false;
        }
        section.size = span;
        section.characteristics = raw.Characteristics;
        image.sections.push_back(std::move(section));
    }
    if (image.sections.empty()) {
        error = "capability: PE image has no bounded memory sections";
        return false;
    }
    std::sort(image.sections.begin(), image.sections.end(),
              [](const PeSection& left, const PeSection& right) { return left.base < right.base; });
    return true;
}

inline bool section_contains(const PeSection& section, uint64_t address,
                             size_t length = 1) noexcept {
    if (address < section.base) {
        return false;
    }
    uint64_t section_end = 0;
    uint64_t address_end = 0;
    return checked_add(section.base, section.size, section_end) &&
           checked_add(address, static_cast<uint64_t>(length), address_end) &&
           address_end >= address && address_end <= section_end;
}

inline const PeSection* find_section(const PeImage& image, uint64_t address,
                                     uint32_t required_characteristics = 0) noexcept {
    for (const PeSection& section : image.sections) {
        if ((section.characteristics & required_characteristics) == required_characteristics &&
            section_contains(section, address)) {
            return &section;
        }
    }
    return nullptr;
}

struct CachedRange final {
    uint64_t base = 0;
    uint32_t characteristics = 0;
    std::vector<uint8_t> bytes;
};

inline bool cache_sections(ReadBudget& reader, const PeImage& image,
                           const std::vector<size_t>& section_indices, uint64_t maximum_bytes,
                           std::vector<CachedRange>& ranges, bool& incomplete) {
    constexpr uint64_t kChunkBytes = 1024ull * 1024ull;
    uint64_t selected_bytes = 0;
    incomplete = false;
    for (const size_t section_index : section_indices) {
        if (section_index >= image.sections.size()) {
            continue;
        }
        const PeSection& section = image.sections[section_index];
        uint64_t section_offset = 0;
        while (section_offset < section.size) {
            if (!reader.checkpoint()) {
                return false;
            }
            if (selected_bytes >= maximum_bytes) {
                incomplete = true;
                return true;
            }
            const uint64_t remaining_section = section.size - section_offset;
            const uint64_t remaining_selection = maximum_bytes - selected_bytes;
            const size_t chunk = static_cast<size_t>((std::min)({
                remaining_section, remaining_selection, kChunkBytes}));
            uint64_t address = 0;
            if (chunk == 0 || !checked_add(section.base, section_offset, address)) {
                incomplete = true;
                return true;
            }
            CachedRange range;
            range.base = address;
            range.characteristics = section.characteristics;
            range.bytes.resize(chunk);
            if (reader.read(address, range.bytes.data(), range.bytes.size())) {
                uint64_t previous_end = 0;
                if (!ranges.empty() &&
                    checked_add(ranges.back().base, ranges.back().bytes.size(), previous_end) &&
                    previous_end == range.base &&
                    ranges.back().characteristics == range.characteristics) {
                    ranges.back().bytes.insert(ranges.back().bytes.end(), range.bytes.begin(),
                                               range.bytes.end());
                } else {
                    ranges.push_back(std::move(range));
                }
            } else if (reader.reason() != StopReason::None) {
                return false;
            } else {
                incomplete = true;
            }
            section_offset += chunk;
            selected_bytes += chunk;
        }
    }
    return true;
}

class MemoryView final {
public:
    MemoryView(ReadBudget& reader, const std::vector<CachedRange>& ranges)
        : reader_(reader), ranges_(ranges) {}

    bool read(uint64_t address, void* output, size_t length) noexcept {
        if ((length != 0 && output == nullptr) || !valid_user_range(address, length)) {
            return false;
        }
        for (const CachedRange& range : ranges_) {
            if (address < range.base) {
                continue;
            }
            const uint64_t offset = address - range.base;
            if (offset <= range.bytes.size() && length <= range.bytes.size() - offset) {
                std::memcpy(output, range.bytes.data() + static_cast<size_t>(offset), length);
                return true;
            }
        }
        return reader_.read(address, output, length);
    }

    template <typename Value>
    bool read_value(uint64_t address, Value& output) noexcept {
        output = {};
        return read(address, &output, sizeof(output));
    }

    bool read_pointer(uint64_t address, uint8_t pointer_size, uint64_t& output) noexcept {
        output = 0;
        if (pointer_size == 8) {
            return read_value(address, output);
        }
        if (pointer_size == 4) {
            uint32_t value = 0;
            if (!read_value(address, value)) {
                return false;
            }
            output = value;
            return true;
        }
        return false;
    }

private:
    ReadBudget& reader_;
    const std::vector<CachedRange>& ranges_;
};

inline bool read_bounded_cstring(MemoryView& memory, uint64_t address, size_t maximum_length,
                                 std::string& output) {
    if (!valid_user_range(address, 1) || maximum_length == 0 || maximum_length > 1024) {
        return false;
    }
    output.clear();
    std::array<char, 32> chunk{};
    size_t offset = 0;
    while (offset < maximum_length) {
        uint64_t chunk_address = 0;
        if (!checked_add(address, offset, chunk_address)) {
            return false;
        }
        const size_t page_remaining = 0x1000u - static_cast<size_t>(chunk_address & 0xfffu);
        const size_t amount =
            (std::min)({chunk.size(), maximum_length - offset, page_remaining});
        if (amount == 0 ||
            !memory.read(chunk_address, chunk.data(), amount)) {
            return false;
        }
        for (size_t index = 0; index < amount; ++index) {
            if (chunk[index] == '\0') {
                return true;
            }
            output.push_back(chunk[index]);
        }
        offset += amount;
    }
    output.clear();
    return false;
}

inline bool read_bounded_module_cstring(MemoryView& memory, const ModuleInfo& module,
                                        uint64_t address, size_t maximum_length,
                                        std::string& output) {
    uint64_t module_end = 0;
    if (maximum_length == 0 || !module_contains(module, address) ||
        !checked_add(module.base, module.size, module_end) || address >= module_end) {
        return false;
    }
    const uint64_t available = module_end - address;
    const size_t bounded_length = available < maximum_length
                                      ? static_cast<size_t>(available)
                                      : maximum_length;
    return read_bounded_cstring(memory, address, bounded_length, output);
}

inline bool valid_utf8_text(std::string_view value, size_t maximum_length) noexcept {
    if (value.empty() || value.size() > maximum_length ||
        !sao::ai_editor::native::valid_utf8(value)) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7f) {
            return false;
        }
    }
    return true;
}

inline bool valid_ascii_identifier(std::string_view value, size_t maximum_length,
                                   bool require_dt_prefix = false) noexcept {
    if (value.empty() || value.size() > maximum_length ||
        (require_dt_prefix && !value.starts_with("DT_"))) {
        return false;
    }
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '[' || character == ']' || character == '.') {
            continue;
        }
        return false;
    }
    return true;
}

inline std::string escaped_field(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character == '\t') {
            result.append("\\t");
        } else if (character == '\r') {
            result.append("\\r");
        } else if (character == '\n') {
            result.append("\\n");
        } else if (character < 0x20u || character == 0x7fu) {
            result.append("\\x");
            result.push_back(digits[character >> 4u]);
            result.push_back(digits[character & 0x0fu]);
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

inline std::string hexadecimal(uint64_t value, size_t digits) {
    static constexpr char kDigits[] = "0123456789abcdef";
    if (digits == 0 || digits > 16) {
        return {};
    }
    std::string result(digits + 2, '0');
    result[0] = '0';
    result[1] = 'x';
    for (size_t index = 0; index < digits; ++index) {
        const size_t shift = (digits - index - 1) * 4;
        result[index + 2] = kDigits[(value >> shift) & 0xfu];
    }
    return result;
}

inline std::string decimal(uint64_t value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return converted.ec == std::errc() ? std::string(buffer.data(), converted.ptr) : std::string();
}

inline std::string signed_decimal(int64_t value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return converted.ec == std::errc() ? std::string(buffer.data(), converted.ptr) : std::string();
}

inline bool append_output(std::string& output, std::string_view line, std::string& error) {
    if (output.size() >= kMaximumOutputBytes ||
        line.size() > kMaximumOutputBytes - output.size() - 1) {
        error = "budget: deterministic output exceeds 32 MiB";
        return false;
    }
    output.append(line);
    output.push_back('\n');
    return true;
}

inline bool reject_reparse_directory_chain(const fs::path& path) {
    fs::path current = path.root_path();
    if (current.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                       FILE_ATTRIBUTE_DEVICE)) !=
            FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    for (const fs::path& component : path.relative_path()) {
        if (component.empty() || component == L"." || component == L"..") {
            return false;
        }
        current /= component;
        attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                           FILE_ATTRIBUTE_DEVICE)) !=
                FILE_ATTRIBUTE_DIRECTORY) {
            return false;
        }
    }
    return true;
}

inline bool path_within_boundary(const fs::path& root, const fs::path& candidate) {
    auto root_component = root.begin();
    auto candidate_component = candidate.begin();
    while (root_component != root.end()) {
        if (candidate_component == candidate.end() ||
            _wcsicmp(root_component->c_str(), candidate_component->c_str()) != 0) {
            return false;
        }
        ++root_component;
        ++candidate_component;
    }
    return true;
}

inline bool prepare_output_path(std::string_view workspace_root,
                                std::string_view output_dir, const wchar_t* fixed_name,
                                fs::path& boundary, fs::path& output,
                                std::string& error) {
    if (workspace_root.empty() || workspace_root.find('\0') != std::string_view::npos ||
        !sao::ai_editor::native::valid_utf8(workspace_root) || output_dir.empty() ||
        output_dir.find('\0') != std::string_view::npos ||
        !sao::ai_editor::native::valid_utf8(output_dir) || fixed_name == nullptr ||
        *fixed_name == L'\0') {
        error = "invalid: workspace_root or output_dir is empty or is not valid UTF-8";
        return false;
    }
    const std::wstring boundary_wide =
        sao::ai_editor::native::utf8_to_wide(workspace_root);
    const std::wstring wide = sao::ai_editor::native::utf8_to_wide(output_dir);
    if (boundary_wide.empty() || wide.empty()) {
        error = "invalid: workspace_root or output_dir cannot be converted to a Windows path";
        return false;
    }
    std::error_code filesystem_error;
    fs::path requested_boundary(boundary_wide);
    requested_boundary = requested_boundary.is_absolute()
                             ? requested_boundary
                             : fs::absolute(requested_boundary, filesystem_error);
    if (filesystem_error || !requested_boundary.is_absolute()) {
        error = "invalid: workspace_root cannot be made absolute";
        return false;
    }
    requested_boundary = requested_boundary.lexically_normal();
    if (!reject_reparse_directory_chain(requested_boundary)) {
        error = "invalid: workspace_root must be an existing non-reparse directory chain";
        return false;
    }
    boundary = fs::canonical(requested_boundary, filesystem_error);
    if (filesystem_error || !boundary.is_absolute() ||
        !reject_reparse_directory_chain(boundary)) {
        error = "invalid: workspace_root canonicalization failed";
        return false;
    }

    filesystem_error.clear();
    fs::path requested(wide);
    requested = requested.is_absolute() ? requested : fs::absolute(requested, filesystem_error);
    if (filesystem_error || !requested.is_absolute()) {
        error = "invalid: output_dir cannot be made absolute";
        return false;
    }
    requested = requested.lexically_normal();
    if (!reject_reparse_directory_chain(requested)) {
        error = "invalid: output_dir must be an existing non-reparse directory chain";
        return false;
    }
    const fs::path root = fs::canonical(requested, filesystem_error);
    if (filesystem_error || !root.is_absolute() || !reject_reparse_directory_chain(root) ||
        !path_within_boundary(boundary, root)) {
        error = "invalid: output_dir canonicalization failed";
        return false;
    }
    output = (root / fixed_name).lexically_normal();
    if (output.parent_path() != root || output.filename() != fs::path(fixed_name)) {
        error = "invalid: fixed output path escapes output_dir";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(output.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                       FILE_ATTRIBUTE_DEVICE)) != 0) {
        error = "invalid: fixed output path is not a regular non-reparse file";
        return false;
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD os_error = GetLastError();
        if (os_error != ERROR_FILE_NOT_FOUND && os_error != ERROR_PATH_NOT_FOUND) {
            error = "capability: fixed output path cannot be inspected";
            return false;
        }
    }
    return true;
}

inline bool publish_output(const fs::path& boundary, const fs::path& output,
                           std::string_view bytes, std::string& error) {
    std::string expected;
    const int32_t read_status = sao::ai_editor::native::read_text_file_bounded(
        boundary, output, static_cast<uint32_t>(kMaximumOutputBytes), expected);
    const int32_t status =
        read_status == SAO_AI_EDITOR_OK
            ? sao::ai_editor::native::write_text_atomic_bounded_if_unchanged(
                  boundary, output, expected, bytes)
        : read_status == SAO_AI_EDITOR_ERR_NOT_FOUND
            ? sao::ai_editor::native::create_text_atomic_bounded(boundary, output, bytes)
            : read_status;
    if (status == SAO_AI_EDITOR_OK) {
        return true;
    }
    if (status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        error = "budget: existing output exceeds 32 MiB";
    } else if (status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT ||
        status == SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION) {
        error = "invalid: output path containment or reparse validation failed";
    } else if (status == SAO_AI_EDITOR_ERR_BUSY) {
        error = "capability: output file changed concurrently before atomic replace";
    } else {
        error = "capability: CREATE_NEW temporary output could not be flushed and atomically replaced";
    }
    return false;
}

inline std::string path_utf8(const fs::path& path) {
    return sao::ai_editor::native::wide_to_utf8(path.native());
}

} // namespace sao::ai_editor::sdk_dumper::detail
