#include "sao/core/thread.h"

#ifdef _WIN32
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Task {
    sao_core_task_fn_t callback;
    void* user_data;
};

class ThreadPool {
public:
    ~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_ready_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    sao_status_t configure(uint32_t worker_count) {
        if (worker_count == 0) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard lock(mutex_);
        if (!workers_.empty()) {
            return SAO_STATUS_OK;
        }
        configured_workers_ = worker_count;
        return SAO_STATUS_OK;
    }

    sao_status_t submit(sao_core_task_fn_t task, void* user_data) {
        if (task == nullptr) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        try {
            std::unique_lock lock(mutex_);
            const sao_status_t start_status = start_locked(lock);
            if (start_status != SAO_STATUS_OK) {
                return start_status;
            }
            tasks_.push_back(Task{task, user_data});
            lock.unlock();
            work_ready_.notify_one();
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t drain() {
        std::unique_lock lock(mutex_);
        drained_.wait(lock, [this] { return tasks_.empty() && active_count_ == 0; });
        return SAO_STATUS_OK;
    }

private:
    sao_status_t start_locked(std::unique_lock<std::mutex>& lock) {
        if (!workers_.empty()) {
            return SAO_STATUS_OK;
        }
        const uint32_t detected = std::thread::hardware_concurrency();
        const uint32_t worker_count = configured_workers_ != 0
            ? configured_workers_
            : (detected != 0 ? detected : 1);
        try {
            workers_.reserve(worker_count);
            for (uint32_t index = 0; index < worker_count; ++index) {
                workers_.emplace_back([this] { worker_loop(); });
            }
            return SAO_STATUS_OK;
        } catch (...) {
            stopping_ = true;
            work_ready_.notify_all();
            lock.unlock();
            for (std::thread& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            lock.lock();
            workers_.clear();
            stopping_ = false;
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    void worker_loop() {
        for (;;) {
            Task task{};
            {
                std::unique_lock lock(mutex_);
                work_ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = tasks_.front();
                tasks_.pop_front();
                ++active_count_;
            }
            try {
                task.callback(task.user_data);
            } catch (...) {
            }
            {
                std::lock_guard lock(mutex_);
                --active_count_;
                if (tasks_.empty() && active_count_ == 0) {
                    drained_.notify_all();
                }
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable drained_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    uint32_t configured_workers_ = 0;
    size_t active_count_ = 0;
    bool stopping_ = false;
};

ThreadPool& shared_pool() {
    static ThreadPool pool;
    return pool;
}

}  // namespace

struct sao_core_timer_s {
    std::mutex mutex;
    std::condition_variable stopped;
    bool stop_requested = false;
    uint32_t interval_ms = 0;
    sao_core_task_fn_t callback = nullptr;
    void* user_data = nullptr;
    std::thread worker;
};

namespace {

void timer_loop(sao_core_timer_s* timer) {
    std::unique_lock lock(timer->mutex);
    while (!timer->stopped.wait_for(
        lock,
        std::chrono::milliseconds(timer->interval_ms),
        [timer] { return timer->stop_requested; })) {
        const sao_core_task_fn_t callback = timer->callback;
        void* user_data = timer->user_data;
        lock.unlock();
        try {
            callback(user_data);
        } catch (...) {
        }
        lock.lock();
    }
}

}  // namespace
#endif

extern "C" sao_status_t SAO_CORE_CALL sao_core_thread_pool_configure(
    uint32_t worker_count) {
#ifdef _WIN32
    try {
        return shared_pool().configure(worker_count);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)worker_count;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_thread_pool_submit(
    sao_core_task_fn_t task, void* user_data) {
#ifdef _WIN32
    try {
        return shared_pool().submit(task, user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    (void)task;
    (void)user_data;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_thread_pool_drain(void) {
#ifdef _WIN32
    try {
        return shared_pool().drain();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_timer_create(
    uint32_t interval_ms,
    sao_core_task_fn_t callback,
    void* user_data,
    sao_core_timer_handle_t* out_timer) {
    if (out_timer == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_timer = nullptr;
#ifdef _WIN32
    if (interval_ms == 0 || callback == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* timer = new (std::nothrow) sao_core_timer_s{};
    if (timer == nullptr) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    timer->interval_ms = interval_ms;
    timer->callback = callback;
    timer->user_data = user_data;
    try {
        timer->worker = std::thread(timer_loop, timer);
    } catch (...) {
        delete timer;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_timer = timer;
    return SAO_STATUS_OK;
#else
    (void)interval_ms;
    (void)callback;
    (void)user_data;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" void SAO_CORE_CALL sao_core_timer_destroy(
    sao_core_timer_handle_t timer) {
#ifdef _WIN32
    if (timer == nullptr) {
        return;
    }
    {
        std::lock_guard lock(timer->mutex);
        timer->stop_requested = true;
    }
    timer->stopped.notify_all();
    if (timer->worker.joinable()) {
        timer->worker.join();
    }
    delete timer;
#else
    (void)timer;
#endif
}

extern "C" uint32_t SAO_CORE_CALL sao_core_thread_current_id(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

extern "C" void SAO_CORE_CALL sao_core_thread_yield(void) {
#ifdef _WIN32
    SwitchToThread();
#endif
}
