// selective_parsing.cpp — per-event record/skip policy (Phase 14).
// Port of act_platform/selective_parsing.py. Modes: all / self / party /
// include(list) / exclude(list). Applied by parser adapters before events
// reach the event bus.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace sao::plugins::loader::selective_parsing {

enum class Mode { All, SelfOnly, PartyOnly, Include, Exclude };

struct Filter {
    Mode mode = Mode::All;
    std::unordered_set<std::string> ids;
};

std::mutex g_mu;
Filter g_filter;

extern "C" void sao_plugins_selective_parsing_set_mode(int mode) {
    std::lock_guard lock(g_mu);
    g_filter.mode = static_cast<Mode>(mode);
}

extern "C" void sao_plugins_selective_parsing_add_id(const char* id) {
    if (id == nullptr) return;
    std::lock_guard lock(g_mu);
    g_filter.ids.insert(id);
}

extern "C" int sao_plugins_selective_parsing_allows(const char* actor_id,
                                                     int is_self, int is_party) {
    std::lock_guard lock(g_mu);
    switch (g_filter.mode) {
    case Mode::All:       return 1;
    case Mode::SelfOnly:  return is_self ? 1 : 0;
    case Mode::PartyOnly: return (is_self || is_party) ? 1 : 0;
    case Mode::Include:
        return (actor_id && g_filter.ids.count(actor_id)) ? 1 : 0;
    case Mode::Exclude:
        return (actor_id && g_filter.ids.count(actor_id)) ? 0 : 1;
    }
    return 1;
}

} // namespace sao::plugins::loader::selective_parsing
