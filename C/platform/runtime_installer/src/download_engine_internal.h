// SAO Auto -- runtime installer download engine internal header.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/runtime_installer/runtime_installer.h"

namespace sao::runtime_installer::internal {

// Sink adapter -- the download loop pushes bytes here as they arrive from
// WinHTTP or the test transport.  Implementations plumb the bytes into the
// hash contexts and the staging file.
class DownloadSink {
public:
    virtual ~DownloadSink() = default;
    virtual sao_status_t write(const uint8_t* bytes, size_t length) noexcept = 0;
    // Called after a successful download so implementations can flush /
    // record the total length.  Not called on error.
    virtual void commit_length(uint64_t total_bytes) noexcept = 0;
};

sao_status_t stream_download(const char* url_utf8,
                             DownloadSink& sink,
                             uint64_t max_bytes,
                             sao_runtime_installer_progress_cb_t progress_cb,
                             void* user_data) noexcept;

}  // namespace sao::runtime_installer::internal
