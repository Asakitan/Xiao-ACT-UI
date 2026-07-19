#include "sao_core/sao_core.h"

// Historical names are exported as PRIVATE entries by legacy_binary_compat.def.
// Keeping them out of the import library prevents them from satisfying platform
// core references in new combined links while preserving runtime DLL lookup.
#undef SAO_LEGACY_CORE_API
#define SAO_LEGACY_CORE_API

using sao_core_process_handle_t = sao_legacy_core_process_handle_t;
using SaoCoreProcessInfo = SaoLegacyCoreProcessInfo;
using SaoCoreBgraPixel = SaoLegacyCoreBgraPixel;
using SaoCoreWindowInfo = SaoLegacyCoreWindowInfo;
using sao_log_callback_t = sao_legacy_core_log_callback_t;
using sao_core_structured_log_callback_t = sao_legacy_core_structured_log_callback_t;

extern "C" SAO_LEGACY_CORE_API uint32_t SAO_LEGACY_CORE_CALL sao_core_abi_version(void) {
    return sao_legacy_core_abi_version();
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_process_open(uint32_t pid, sao_core_process_handle_t* out_handle) {
    return sao_legacy_core_process_open(pid, out_handle);
}

extern "C" SAO_LEGACY_CORE_API void SAO_LEGACY_CORE_CALL
sao_core_process_close(sao_core_process_handle_t handle) {
    sao_legacy_core_process_close(handle);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_process_get_info(sao_core_process_handle_t handle, SaoCoreProcessInfo* out_info,
                          char* out_image_path_utf8, size_t image_path_capacity) {
    return sao_legacy_core_process_get_info(handle, out_info, out_image_path_utf8,
                                            image_path_capacity);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_read_bytes(sao_core_process_handle_t handle, uint64_t address, uint8_t* out_buffer,
                    size_t buffer_len, size_t* out_bytes_read) {
    return sao_legacy_core_read_bytes(handle, address, out_buffer, buffer_len, out_bytes_read);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_class_index_resolve(
    sao_core_process_handle_t handle, const char* class_name_utf8, uint64_t* out_class_ptr) {
    return sao_legacy_core_class_index_resolve(handle, class_name_utf8, out_class_ptr);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_class_index_resolve_field_offset(sao_core_process_handle_t handle, uint64_t class_ptr,
                                          const char* field_name_utf8, uint32_t* out_offset) {
    return sao_legacy_core_class_index_resolve_field_offset(handle, class_ptr, field_name_utf8,
                                                            out_offset);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_class_index_register(const char* class_name_utf8, uint32_t* out_index) {
    return sao_legacy_core_class_index_register(class_name_utf8, out_index);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_class_index_find(const char* class_name_utf8, uint32_t* out_index) {
    return sao_legacy_core_class_index_find(class_name_utf8, out_index);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_class_index_get_name(uint32_t index, char* out_class_name_utf8, size_t class_name_capacity,
                              size_t* out_required_size) {
    return sao_legacy_core_class_index_get_name(index, out_class_name_utf8, class_name_capacity,
                                                out_required_size);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_class_index_count(size_t* out_count) {
    return sao_legacy_core_class_index_count(out_count);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_scan_find_pattern(const uint8_t* haystack, size_t haystack_len, const uint8_t* pattern,
                           const uint8_t* mask, size_t pattern_len, size_t* out_offset) {
    return sao_legacy_core_scan_find_pattern(haystack, haystack_len, pattern, mask, pattern_len,
                                             out_offset);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_scan_find_aligned_u64(
    const uint8_t* haystack, size_t haystack_len, const uint64_t* target_values,
    size_t target_count, size_t* out_offsets, size_t max_out_offsets, size_t* out_match_count) {
    return sao_legacy_core_scan_find_aligned_u64(haystack, haystack_len, target_values,
                                                 target_count, out_offsets, max_out_offsets,
                                                 out_match_count);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_pixels_premultiply_blend(const uint8_t* src_bgra, uint8_t* dst_bgra, uint32_t width,
                                  uint32_t height, uint32_t stride_bytes) {
    return sao_legacy_core_pixels_premultiply_blend(src_bgra, dst_bgra, width, height,
                                                    stride_bytes);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_pixels_find_alpha_spans(
    const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t row_index,
    uint32_t* out_spans_x0x1, uint32_t max_spans, uint32_t* out_span_count) {
    return sao_legacy_core_pixels_find_alpha_spans(bgra, width, height, stride_bytes, row_index,
                                                   out_spans_x0x1, max_spans, out_span_count);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_pixels_validate_region(
    size_t buffer_len, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t x,
    uint32_t y, uint32_t region_width, uint32_t region_height, size_t* out_first_byte_offset,
    size_t* out_required_end_offset) {
    return sao_legacy_core_pixels_validate_region(buffer_len, width, height, stride_bytes, x, y,
                                                  region_width, region_height,
                                                  out_first_byte_offset, out_required_end_offset);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_pixels_sample_bgra(
    const uint8_t* bgra, size_t buffer_len, uint32_t width, uint32_t height, uint32_t stride_bytes,
    uint32_t x, uint32_t y, SaoCoreBgraPixel* out_pixel) {
    return sao_legacy_core_pixels_sample_bgra(bgra, buffer_len, width, height, stride_bytes, x, y,
                                              out_pixel);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_window_create_layered_topmost(
    const wchar_t* title, int32_t x, int32_t y, int32_t w, int32_t h, void** out_hwnd) {
    return sao_legacy_core_window_create_layered_topmost(title, x, y, w, h, out_hwnd);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_core_window_destroy(void* hwnd) {
    return sao_legacy_core_window_destroy(hwnd);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_window_enumerate(void** out_hwnds, size_t max_hwnds, size_t* out_hwnd_count) {
    return sao_legacy_core_window_enumerate(out_hwnds, max_hwnds, out_hwnd_count);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_window_get_info(void* hwnd, SaoCoreWindowInfo* out_info) {
    return sao_legacy_core_window_get_info(hwnd, out_info);
}

extern "C" SAO_LEGACY_CORE_API void SAO_LEGACY_CORE_CALL
sao_core_set_log_callback(sao_log_callback_t callback) {
    sao_legacy_core_set_log_callback(callback);
}

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_core_set_structured_log_callback(sao_core_structured_log_callback_t callback, void* user_data) {
    return sao_legacy_core_set_structured_log_callback(callback, user_data);
}
