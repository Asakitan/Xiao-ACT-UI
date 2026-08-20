#pragma once

#include "sao/core/status.h"
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace sao::launcher::settings_owner {
using Json = nlohmann::ordered_json;
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
  private:
    sao_status_t set_value_locked(std::string_view top_level_key, Json value) noexcept;
    sao_status_t save_locked() noexcept;
    mutable std::mutex mutex_;
    std::wstring path_;
    std::wstring registry_path_;
    bool registry_registered_ = false;
    Json document_ = Json::object();
    bool dirty_ = false;
    std::shared_ptr<Lease::LeaseState> lease_state_;
};
} // namespace sao::launcher::settings_owner