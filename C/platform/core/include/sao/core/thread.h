// SAO Auto — thread + thread-pool primitives.
//
// The Python side leans on `threading.Thread` + one-off pool objects.
// Here we ship one shared work-stealing pool for CPU work (memory scan
// batches, JSON encode, hash) and a dedicated timer thread for periodic
// callbacks.
//
// All entry points are safe to call from any thread.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (SAO_CORE_CALL* sao_core_task_fn_t)(void* user_data);

// The shared pool — sized to std::thread::hardware_concurrency by
// default.  Users may override via `sao_core_thread_pool_configure` at
// startup; changes are ignored after the first submit.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_thread_pool_configure(
    uint32_t worker_count);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_thread_pool_submit(
    sao_core_task_fn_t task, void* user_data);

// Blocks until *all* submitted work completes.  Do not call from a task.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_thread_pool_drain(void);

// Periodic timer — invokes callback every interval_ms until the returned
// handle is destroyed.  callback runs on a dedicated timer thread; keep
// the work short and marshal to the pool for heavy jobs.
typedef struct sao_core_timer_s* sao_core_timer_handle_t;

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_timer_create(
    uint32_t interval_ms,
    sao_core_task_fn_t callback,
    void* user_data,
    sao_core_timer_handle_t* out_timer);

SAO_CORE_API void SAO_CORE_CALL sao_core_timer_destroy(
    sao_core_timer_handle_t timer);

// Uniquely identify the calling OS thread.
SAO_CORE_API uint32_t SAO_CORE_CALL sao_core_thread_current_id(void);

// Cooperative yield hint to the scheduler — equivalent to Sleep(0).
SAO_CORE_API void SAO_CORE_CALL sao_core_thread_yield(void);

#ifdef __cplusplus
}  // extern "C"
#endif
