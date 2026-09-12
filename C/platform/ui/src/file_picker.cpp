#include "sao/ui/file_picker.h"
#include "sao/ui/panel.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;
namespace fs = std::filesystem;

struct sao_ui_file_picker_s {
	sao_ui_compositor_handle_t compositor{};
	sao_ui_panel_handle_t panel{};
	sao_ui_file_picker_result_fn_t callback{};
	void* callback_data{};
	fs::path directory;
	std::vector<fs::directory_entry> entries;
	std::string location, name, title, filter, error;
	size_t page{};
	int32_t mode{};
	bool visible{}, callback_active{}, overwrite_armed{}, truncated{};
};

namespace {
constexpr size_t kPageSize = 24;
std::atomic<uint64_t> g_picker_id{};
std::string utf8(const fs::path& path) {
	const auto text = path.u8string();
	return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path path_from_utf8(const std::string& value) {
	return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
json text(std::string value, const char* style = "value", int height = 28) {
	return {{"type", "text"}, {"text", std::move(value)}, {"style", style}, {"wrap", true}, {"height", height}};
}
json button(std::string id, std::string label, std::string action, json payload = json::object(), bool disabled = false) {
	if (payload.is_null()) payload = json::object();
	return {{"type", "button"}, {"id", std::move(id)}, {"label", std::move(label)},
		{"action", std::move(action)}, {"payload", std::move(payload)}, {"disabled", disabled}, {"height", 36}};
}
json input(const char* id, const std::string& value, const char* action) {
	return {{"type", "input"}, {"id", id}, {"value", value}, {"action", action}, {"height", 38}};
}
sao_status_t publish(sao_ui_file_picker_s& state) {
	json rows = json::array();
	const size_t begin = std::min(state.page * kPageSize, state.entries.size());
	for (size_t i = begin; i < std::min(begin + kPageSize, state.entries.size()); ++i) {
		std::error_code ec;
		const bool directory = state.entries[i].is_directory(ec);
		rows.push_back(button("file.entry." + std::to_string(i),
			(directory ? "目录  /  " : "文件  /  ") + utf8(state.entries[i].path().filename()),
			"file.entry", {{"index", i}}));
	}
	if (rows.empty()) rows.push_back(text("此目录没有可显示的文件。", "muted", 60));
	const std::string selected_label = state.mode == SAO_UI_FILE_PICKER_SAVE ?
		(state.overwrite_armed ? "确认覆盖" : "保存") : state.mode == SAO_UI_FILE_PICKER_FOLDER ? "选择此目录" : "打开";
	const json document{{"version", 1}, {"layout", "dock"}, {"nodes", json::array({
		{{"type", "section"}, {"id", "file.header"}, {"dock", "top"}, {"children", json::array({
			text(state.title, "title", 30), input("file.location", state.location, "file.location"),
			{{"type", "row"}, {"children", json::array({button("file.up", "上一级", "file.up"),
				button("file.go", "转到目录", "file.go"), button("file.refresh", "刷新", "file.go")})}}
		})}},
		{{"type", "section"}, {"id", "file.list"}, {"dock", "fill"}, {"weight", 1.0},
		 {"scroll", {{"axis", "vertical"}, {"bar", "auto"}, {"wheel", true}}}, {"children", std::move(rows)}},
		{{"type", "section"}, {"id", "file.footer"}, {"dock", "bottom"}, {"children", json::array({
			{{"type", "row"}, {"children", json::array({button("file.previous", "上一页", "file.previous", {}, state.page == 0),
				text(std::to_string(begin) + "–" + std::to_string(std::min(begin + kPageSize, state.entries.size())) +
					" / " + std::to_string(state.entries.size()), "muted"),
				button("file.next", "下一页", "file.next", {}, begin + kPageSize >= state.entries.size())})}},
			text("文件名或完整路径", "muted", 22),
			input("file.name", state.name, "file.name"),
			text(state.error.empty() ? (state.truncated ? "目录已达显示上限，可输入完整路径选择其他文件。" :
				state.filter.empty() ? "选择文件或输入完整路径。" : "文件类型：" + state.filter) : state.error,
				state.error.empty() ? "muted" : "warn", 42),
			{{"type", "row"}, {"children", json::array({button("file.accept", selected_label, "file.accept"),
				button("file.cancel", "取消", "file.cancel")})}}
		})}}
	})}};
	const auto bytes = document.dump();
	return sao_ui_panel_set_spec(state.panel, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

bool scan(sao_ui_file_picker_s& state, const fs::path& directory) {
	std::error_code ec;
	if (!fs::is_directory(directory, ec) || ec) { state.error = "目录不存在或暂时不可访问。"; return false; }
	std::vector<fs::directory_entry> entries;
	fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
	if (ec) { state.error = "读取目录失败。"; return false; }
	while (it != end && entries.size() < 4096) {
		entries.push_back(*it); it.increment(ec);
		if (ec) break;
	}
	std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
		std::error_code a, b;
		const bool ld = left.is_directory(a), rd = right.is_directory(b);
		return ld != rd ? ld : left.path().filename().native() < right.path().filename().native();
	});
	state.truncated = it != end;
	state.entries = std::move(entries); state.directory = directory;
	state.location = utf8(directory); state.page = 0; state.overwrite_armed = false;
	state.error = ec ? "部分目录项未能读取。" : "";
	return true;
}

void finish(sao_ui_file_picker_s& state, sao_status_t status, const std::string& path) {
	if (!state.visible) return;
	state.visible = false;
	(void)sao_ui_panel_set_visible(state.panel, false);
	const auto callback = state.callback;
	void* data = state.callback_data;
	state.callback = nullptr; state.callback_data = nullptr;
	state.callback_active = true;
	if (callback) { try { callback(status, path.c_str(), data); } catch (...) {} }
	state.callback_active = false;
}

void SAO_UI_CALL action(const char* key, const uint8_t* data, size_t size, void* user) {
	auto& state = *static_cast<sao_ui_file_picker_s*>(user);
	if (!state.visible || !key || sao_ui_compositor_require_owner_thread(state.compositor) != SAO_STATUS_OK) return;
	try {
		const std::string command(key);
		const json payload = size ? json::parse(data, data + size, nullptr, false) : json::object();
		if (!payload.is_object()) return;
		if (command == "file.cancel") { finish(state, SAO_STATUS_ERR_CANCELLED, {}); return; }
		if (command == "file.name" || command == "file.location") {
			const auto value = payload.value("text", payload.value("value", std::string{}));
			if (value.size() > 32768 || value.find('\0') != std::string::npos) return;
			(command == "file.name" ? state.name : state.location) = value;
			state.overwrite_armed = false;
		} else if (command == "file.go") scan(state, path_from_utf8(state.location));
		else if (command == "file.up") scan(state, state.directory.parent_path());
		else if (command == "file.previous" && state.page) --state.page;
		else if (command == "file.next" && (state.page + 1) * kPageSize < state.entries.size()) ++state.page;
		else if (command == "file.entry") {
			const size_t index = payload.value("index", state.entries.size());
			if (index >= state.entries.size()) return;
			const auto entry = state.entries[index];
			std::error_code ec;
			if (entry.is_directory(ec)) scan(state, entry.path());
			else { state.name = utf8(entry.path().filename()); state.overwrite_armed = false; }
		} else if (command == "file.accept") {
			fs::path chosen = state.mode == SAO_UI_FILE_PICKER_FOLDER ? state.directory :
				state.directory / path_from_utf8(state.name);
			std::error_code ec;
			if (state.mode == SAO_UI_FILE_PICKER_FOLDER && fs::is_directory(chosen, ec)) {
				finish(state, SAO_STATUS_OK, utf8(chosen)); return;
			}
			if (state.mode == SAO_UI_FILE_PICKER_OPEN && fs::is_regular_file(chosen, ec)) {
				finish(state, SAO_STATUS_OK, utf8(chosen)); return;
			}
			if (state.mode == SAO_UI_FILE_PICKER_SAVE && !state.name.empty() && !chosen.filename().empty() &&
				fs::is_directory(chosen.parent_path(), ec) && !fs::is_directory(chosen, ec)) {
				ec.clear();
				const bool exists = fs::exists(chosen, ec);
				if (ec) state.error = "无法确认目标文件状态。";
				else if (exists && !state.overwrite_armed) {
					state.overwrite_armed = true; state.error = "目标文件已存在。再次点击“确认覆盖”后才会覆盖。";
				} else { finish(state, SAO_STATUS_OK, utf8(chosen)); return; }
			} else state.error = "请选择有效的文件或目录。";
		}
		(void)publish(state);
	} catch (...) { state.error = "路径或文件选择参数无效。"; try { (void)publish(state); } catch (...) {} }
}

void SAO_UI_CALL event(int32_t kind, void* data) {
	if (kind == SAO_UI_PANEL_EVENT_CLOSE) finish(*static_cast<sao_ui_file_picker_s*>(data), SAO_STATUS_ERR_CANCELLED, {});
}
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_create(sao_ui_compositor_handle_t compositor, sao_ui_file_picker_handle_t* out) {
	if (!out || !compositor) return SAO_STATUS_ERR_INVALID_ARGUMENT;
	*out = nullptr;
	const auto owner = sao_ui_compositor_require_owner_thread(compositor);
	if (owner != SAO_STATUS_OK) return owner;
	try {
		auto state = std::make_unique<sao_ui_file_picker_s>(); state->compositor = compositor;
		const std::string id = "sao.file-picker." + std::to_string(++g_picker_id);
		SaoPanelConfig config{}; config.panel_id_utf8 = id.c_str(); config.title_utf8 = "文件选择";
		config.default_width = 760; config.default_height = 680; config.default_x = 120; config.default_y = 70;
		config.min_width = 460; config.min_height = 480; config.movable = true; config.resizable = true;
		config.show_titlebar = true; config.show_close_button = true;
		const auto created = sao_ui_panel_create(compositor, &config, &state->panel);
		if (created != SAO_STATUS_OK) return created;
		(void)sao_ui_panel_set_action_handler(state->panel, action, state.get());
		(void)sao_ui_panel_set_event_handler(state->panel, event, state.get());
		(void)sao_ui_layer_set_z_order(sao_ui_panel_layer(state->panel), 2200);
		*out = state.release(); return SAO_STATUS_OK;
	} catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_show(sao_ui_file_picker_handle_t state, const SaoUiFilePickerConfig* config, sao_ui_file_picker_result_fn_t callback, void* data) {
	if (!state || !config || !callback || config->mode < 0 || config->mode > 2) return SAO_STATUS_ERR_INVALID_ARGUMENT;
	const auto owner = sao_ui_compositor_require_owner_thread(state->compositor);
	if (owner != SAO_STATUS_OK) return owner;
	if (state->visible || state->callback_active) return SAO_STATUS_ERR_CANCELLED;
	try {
		state->mode = config->mode; state->title = config->title_utf8 ? config->title_utf8 : "选择文件";
		state->filter = config->filter_utf8 ? config->filter_utf8 : "";
		state->name.clear(); state->error.clear(); state->overwrite_armed = false;
		std::error_code ec;
		auto path = config->initial_path_utf8 && *config->initial_path_utf8 ? path_from_utf8(config->initial_path_utf8) : fs::current_path(ec);
		if (!path.is_absolute()) path = fs::absolute(path, ec);
		if (!fs::is_directory(path, ec)) { state->name = utf8(path.filename()); path = path.parent_path(); }
		if (path.empty()) path = fs::current_path(ec);
		if (!scan(*state, path)) return SAO_STATUS_ERR_NOT_FOUND;
		const auto published = publish(*state);
		if (published != SAO_STATUS_OK) return published;
		const auto shown = sao_ui_panel_set_visible(state->panel, true);
		if (shown != SAO_STATUS_OK) return shown;
		state->callback = callback; state->callback_data = data; state->visible = true;
		return SAO_STATUS_OK;
	} catch (...) { return SAO_STATUS_ERR_INVALID_ARGUMENT; }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_cancel(sao_ui_file_picker_handle_t state) {
	if (!state) return SAO_STATUS_ERR_INVALID_ARGUMENT;
	const auto owner = sao_ui_compositor_require_owner_thread(state->compositor);
	if (owner != SAO_STATUS_OK) return owner;
	finish(*state, SAO_STATUS_ERR_CANCELLED, {}); return SAO_STATUS_OK;
}
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_try_destroy(sao_ui_file_picker_handle_t state) {
	if (!state) return SAO_STATUS_OK;
	const auto owner = sao_ui_compositor_require_owner_thread(state->compositor);
	if (owner != SAO_STATUS_OK) return owner;
	if (state->callback_active) return SAO_STATUS_ERR_CANCELLED;
	finish(*state, SAO_STATUS_ERR_CANCELLED, {});
	(void)sao_ui_panel_set_action_handler(state->panel, nullptr, nullptr);
	(void)sao_ui_panel_set_event_handler(state->panel, nullptr, nullptr);
	sao_ui_panel_destroy(state->panel); delete state; return SAO_STATUS_OK;
}
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_is_visible(sao_ui_file_picker_handle_t state, bool* visible) {
	if (!state || !visible) return SAO_STATUS_ERR_INVALID_ARGUMENT;
	const auto owner = sao_ui_compositor_require_owner_thread(state->compositor);
	if (owner != SAO_STATUS_OK) return owner;
	*visible = state->visible; return SAO_STATUS_OK;
}
