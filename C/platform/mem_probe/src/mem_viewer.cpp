// mem_viewer.cpp — hex/typed viewer helper (Phase 14).
// Not a full UI — just a chunk-read API a plugin panel can drive.

#include "sao/core/memory.h"
#include "sao/core/process.h"
#include "sao/core/status.h"

#include <cstddef>
#include <cstdint>

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_mem_viewer_read(
    sao_core_process_handle_t process, uint64_t address, uint8_t* out_buf,
    size_t buf_len, size_t* out_bytes_read) {
    if (out_bytes_read == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Python limit: 4 KiB per read.
    if (buf_len > 4096) buf_len = 4096;
    return sao_core_mem_read(process, address, out_buf, buf_len, out_bytes_read);
}
