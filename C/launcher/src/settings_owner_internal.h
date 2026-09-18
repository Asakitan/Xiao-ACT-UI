#pragma once

#include "sao/core/status.h"
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sao::launcher::settings_owner {
using Json = nlohmann::ordered_json;

// Change-notification callback: fires synchronously on the committing thread
// after a successful document mutation or save clears the dirty flag. The
// callback runs after the owner mutex is released, so it may read the owner
// (snapshot/get_value); it must not block, must not call back into mutate or
// subscribe/unsubscribe paths, and must not throw.
using change_callback_fn = void (*)(void* user_data) noexcept;

struct LoadInfo {
    bool found = false;
    bool legacy_migrated = false;
    bool recovered_corrupt = false;
    sao_status_t source_status = SAO_STATUS_OK;
    sao_status_t backup_status = SAO_STATUS_OK;
    sao_status_t migration_status = SAO_STATUS_OK;
};
class SettingsOwner final {
  public:
    class Lease final {
      public:
        Lease() noexcept = default;
        ~Lease() noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        SettingsOwner* get() const noexcept { return owner_; }
        SettingsOwner* operator->() const noexcept { return owner_; }
      private:
        struct LeaseState;
        Lease(SettingsOwner* owner, std::shared_ptr<LeaseState> state) noexcept;
        void release() noexcept;
        SettingsOwner* owner_{};
        std::shared_ptr<LeaseState> state_;
        friend class SettingsOwner;
    };
  private:
    struct ConstructionToken final {};
  public:
    static sao_status_t create(std::wstring path, std::unique_ptr<SettingsOwner>& out) noexcept;
    SettingsOwner(ConstructionToken, std::wstring path, std::wstring registry_path);
    ~SettingsOwner() noexcept;
    SettingsOwner(const SettingsOwner&) = delete;
    SettingsOwner& operator=(const SettingsOwner&) = delete;
    SettingsOwner(SettingsOwner&&) = delete;
    SettingsOwner& operator=(SettingsOwner&&) = delete;
    sao_status_t load(LoadInfo& out_info) noexcept;
    sao_status_t save() noexcept;
    sao_status_t snapshot(Json& out) const noexcept;
    sao_status_t get_value(std::string_view top_level_key, Json& out) const noexcept;
    sao_status_t get_truthy(std::string_view top_level_key, bool default_value, bool& out) const noexcept;
    sao_status_t set_value(std::string_view top_level_key, Json value) noexcept;
    sao_status_t set_value_and_save(std::string_view top_level_key, Json value) noexcept;
    sao_status_t restore_snapshot(Json document, bool dirty) noexcept;
    Lease acquire_lease() const noexcept;
    void retire_and_wait() noexcept;
    void resume_after_retire() noexcept;
    bool dirty() const noexcept;
    sao_status_t path(std::wstring& out) const noexcept;
    // Subscribes to committed document/dirty transitions. Re-registering the
    // same (callback, user_data) pair is a no-op. unsubscribe waits for any
    // in-flight dispatch of that callback to return before reporting success,
    // so the caller may free user_data immediately afterwards.
    sao_status_t subscribe_change(change_callback_fn callback, void* user_data) noexcept;
    sao_status_t unsubscribe_change(change_callback_fn callback, void* user_data) noexcept;
  private:
    struct SubscriberEntry {
        change_callback_fn callback{};
        void* user_data{};
    };
    sao_status_t set_value_locked(std::string_view top_level_key, Json value,
                                  bool* out_changed) noexcept;
    sao_status_t save_locked() noexcept;
    void notify_change() noexcept;
    mutable std::mutex mutex_;
    std::wstring path_;
    std::wstring registry_path_;
    bool registry_registered_ = false;
    Json document_ = Json::object();
    bool dirty_ = false;
    std::shared_ptr<Lease::LeaseState> lease_state_;
    std::mutex subscribers_mutex_;
    std::condition_variable subscribers_cv_;
    std::vector<SubscriberEntry> subscribers_;
    std::size_t subscribers_dispatching_ = 0;
};
} // namespace sao::launcher::settings_owner

// Owner-scoped change subscription over the C ABI: the panel and other
// launcher modules bind these to the live owner so committed mutations
// (menu toggles, profile loads, hotkey saves) refresh open surfaces.
extern "C" sao_status_t sao_launcher_settings_owner_subscribe_change(
    void* owner_opaque, sao::launcher::settings_owner::change_callback_fn callback,
    void* user_data) noexcept;
extern "C" sao_status_t sao_launcher_settings_owner_unsubscribe_change(
    void* owner_opaque, sao::launcher::settings_owner::change_callback_fn callback,
    void* user_data) noexcept;