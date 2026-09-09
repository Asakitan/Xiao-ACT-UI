#include "vt_bridge.h"

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/rt_io/proxy/rt_io_proxy.h"
#include "sao/rt_io/proxy/vt_proxy.h"
#include "sao/rt_io/status.h"
#include "sao/rt_io/util/secure_wipe.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::ai_editor::vt {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using RuntimeHandle = sao_rt_io_proxy_runtime_handle_t;
using CallResult = SaoRtIoProxyRuntimeCallResult;

constexpr uint32_t kTimeoutMs = 5000u;
constexpr auto kCacheLifetime = std::chrono::milliseconds(50);
constexpr uint32_t kStateAbsent = 0u;
constexpr uint32_t kStateMapped = 1u;
constexpr uint32_t kStateActive = 2u;
constexpr uint32_t kStateGuestReadonly = 4u;
constexpr uint32_t kStatusExtGuestReadonly = 0x00000001u;
constexpr uint32_t kStatusExtPriorHypervisor = 0x00000002u;
constexpr uint32_t kStatusExtStale = 0x00000020u;
constexpr uint32_t kStatusExtRemoved = 0x00000040u;
constexpr uint32_t kStatusExtDeadmanRestartFailed = 0x00000080u;
constexpr uint32_t kStatusExtTscCompensationUnavailable = 0x00000100u;
constexpr size_t kCapabilityCount = 23u;

constexpr std::array<std::string_view, kCapabilityCount> kCapabilityNames{
	"vpid", "invvpidIndividual", "invvpidSingle", "invvpidAll",
	"invvpidRetainGlobals", "eptExecuteOnly", "eptAccessedDirty",
	"inveptSingle", "inveptAll", "mbec", "apicvSupported", "apicvActive",
	"svmNpt", "svmNrips", "svmLbrv", "avicSupported", "avicActive", "gmet",
	"hiddenMemory", "hideRegion", "pageTableHidden", "guestReadonly",
	"physicalRead"};

constexpr std::array<std::string_view, 4> kProbeItemNames{
	"status", "capabilities", "perf", "hooks"};
constexpr std::array<uint64_t, 4> kProbeItemBits{
	SAO_RT_IO_VT_PROXY_PROBE_ITEM_STATUS,
	SAO_RT_IO_VT_PROXY_PROBE_ITEM_CAPABILITIES,
	SAO_RT_IO_VT_PROXY_PROBE_ITEM_PERF,
	SAO_RT_IO_VT_PROXY_PROBE_ITEM_HOOKS};

std::string hex_u64(uint64_t value) {
	char buffer[16]{};
	const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
	return std::string("0x") + std::string(buffer, converted.ptr);
}

std::string hex_bytes(const uint8_t* bytes, size_t size) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string output;
	output.resize(size * 2u);
	for (size_t index = 0u; index < size; ++index) {
		output[index * 2u] = digits[(bytes[index] >> 4u) & 0x0Fu];
		output[index * 2u + 1u] = digits[bytes[index] & 0x0Fu];
	}
	return output;
}

int hex_digit(char value) noexcept {
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

bool parse_hex_u64(const Json& arguments, std::string_view key,
				   uint64_t& output, Json& error) {
	const std::string field(key);
	if (!arguments.contains(field) || !arguments[field].is_string()) {
		error = Json{{"error", "field must be a 0x-prefixed hexadecimal string"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	const std::string& text = arguments[field].get_ref<const std::string&>();
	if (text.size() < 3u || text.size() > 18u || text[0] != '0' ||
		text[1] != 'x' || text.size() - 2u > 16u) {
		error = Json{{"error", "field must use canonical 0x plus 1..16 hex digits"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	uint64_t value = 0u;
	for (size_t index = 2u; index < text.size(); ++index) {
		const int digit = hex_digit(text[index]);
		if (digit < 0 || value > (std::numeric_limits<uint64_t>::max() -
								  static_cast<uint64_t>(digit)) / 16u) {
			error = Json{{"error", "field contains invalid hexadecimal digits"},
						 {"path", "$" + std::string(".") + field}};
			return false;
		}
		value = value * 16u + static_cast<uint64_t>(digit);
	}
	output = value;
	return true;
}

bool upper_canonical_gva(uint64_t value) noexcept {
	return (value & 0xFFFF800000000000ull) == 0xFFFF800000000000ull;
}

bool checked_inclusive_end(uint64_t start, uint64_t length,
                           uint64_t& end) noexcept {
	if (length == 0u || length - 1u > std::numeric_limits<uint64_t>::max() - start)
		return false;
	end = start + length - 1u;
	return true;
}

bool hook_id_valid(uint64_t value) noexcept {
	const uint64_t slot = value & 0x7Fu;
	return value != 0u && (value >> 7u) != 0u && slot >= 1u && slot <= 64u;
}

bool parse_items_mask(const Json& arguments, uint64_t& output, Json& error) {
	output = 0u;
	if (!arguments.contains("items")) return true;
	if (!arguments["items"].is_string()) {
		error = Json{{"error", "items must be a 0x0..0xf hexadecimal mask string"},
					 {"path", "$.items"}};
		return false;
	}
	const std::string& text = arguments["items"].get_ref<const std::string&>();
	if (text.size() != 3u || text[0] != '0' || text[1] != 'x') {
		error = Json{{"error", "items must be exactly 0x0 through 0xf"},
					 {"path", "$.items"}};
		return false;
	}
	const int digit = hex_digit(text[2]);
	if (digit < 0 || digit > 0x0f) {
		error = Json{{"error", "items must be exactly 0x0 through 0xf"},
					 {"path", "$.items"}};
		return false;
	}
	output = static_cast<uint64_t>(digit);
	return true;
}

bool parse_hex_bytes(const Json& arguments, std::string_view key,
					 size_t maximum_bytes, std::vector<uint8_t>& output,
					 Json& error) {
	sao_rt_io_secure_wipe(output.data(), output.size());
	output.clear();
	struct OutputWipe final {
		std::vector<uint8_t>& output;
		bool keep = false;
		~OutputWipe() {
			if (!keep) {
				sao_rt_io_secure_wipe(output.data(), output.size());
				output.clear();
			}
		}
		void discard() noexcept {
			sao_rt_io_secure_wipe(output.data(), output.size());
			output.clear();
		}
	} wipe{output};
	const std::string field(key);
	if (!arguments.contains(field) || !arguments[field].is_string()) {
		error = Json{{"error", "field must be an even-count hexadecimal byte string"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	const std::string& source = arguments[field].get_ref<const std::string&>();
	size_t offset = 0u;
	if (source.size() >= 2u && source[0] == '0' && source[1] == 'x') {
		offset = 2u;
	} else if (source.size() >= 2u && source[0] == '0' && source[1] == 'X') {
		error = Json{{"error", "hex prefix must be lowercase 0x"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	const size_t digit_count = source.size() - offset;
	if ((digit_count & 1u) != 0u) {
		error = Json{{"error", "hex byte string must contain an even number of digits"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	if (digit_count / 2u > maximum_bytes) {
		error = Json{{"error", "hex byte string exceeds its bounded size"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	output.assign(digit_count / 2u, 0u);
	for (size_t index = 0u; index < output.size(); ++index) {
		const int high = hex_digit(source[offset + index * 2u]);
		const int low = hex_digit(source[offset + index * 2u + 1u]);
		if (high < 0 || low < 0) {
			wipe.discard();
			error = Json{{"error", "hex byte string contains invalid digits"},
						 {"path", "$" + std::string(".") + field}};
			return false;
		}
		output[index] = static_cast<uint8_t>((high << 4) | low);
	}
	wipe.keep = true;
	return true;
}

bool parse_integer_range(const Json& arguments, std::string_view key,
						 uint64_t minimum, uint64_t maximum,
						 uint64_t& output, Json& error) {
	const std::string field(key);
	if (!arguments.contains(field) || !arguments[field].is_number_integer()) {
		error = Json{{"error", "field must be an integer in the permitted range"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	uint64_t value = 0u;
	if (arguments[field].is_number_unsigned()) {
		value = arguments[field].get<uint64_t>();
	} else {
		const int64_t signed_value = arguments[field].get<int64_t>();
		if (signed_value < 0) {
			error = Json{{"error", "field must be an integer in the permitted range"},
						 {"path", "$" + std::string(".") + field}};
			return false;
		}
		value = static_cast<uint64_t>(signed_value);
	}
	if (value < minimum || value > maximum) {
		error = Json{{"error", "field is outside the permitted range"},
					 {"path", "$" + std::string(".") + field}};
		return false;
	}
	output = value;
	return true;
}

bool exact_fields(const Json& arguments,
				  std::initializer_list<std::string_view> allowed,
				  Json& error) {
	if (!arguments.is_object()) {
		error = Json{{"error", "arguments must be an object"}, {"path", "$"}};
		return false;
	}
	for (const auto& entry : arguments.items()) {
		bool found = false;
		for (const std::string_view field : allowed) {
			if (entry.key() == field) {
				found = true;
				break;
			}
		}
		if (!found) {
			error = Json{{"error", "unknown field is not allowed"},
						 {"path", "$" + std::string(".") + entry.key()}};
			return false;
		}
	}
	return true;
}

bool confirmed_true(const Json& arguments, Json& error) {
	if (!arguments.contains("confirmed") ||
		!arguments["confirmed"].is_boolean() ||
		!arguments["confirmed"].get<bool>()) {
		error = Json{{"error", "confirmed must be the literal boolean true"},
					 {"path", "$.confirmed"}};
		return false;
	}
	return true;
}

std::string state_name(uint32_t state) {
	switch (state) {
		case kStateAbsent: return "absent";
		case kStateMapped: return "mapped";
		case kStateActive: return "active";
		case kStateGuestReadonly: return "guestReadonly";
		default: return "unknown";
	}
}

std::string availability_reason_name(uint32_t value) {
	switch (value) {
		case 0u: return "none";
		case 1u: return "priorMicrosoftHypervisor";
		case 2u: return "priorForeignHypervisor";
		case 3u: return "cetActive";
		case 4u: return "fredActive";
		case 5u: return "la57Active";
		default: return "unknown";
	}
}

std::string prior_hypervisor_name(uint32_t value) {
	switch (value) {
		case 0u: return "none";
		case 1u: return "microsoft";
		case 2u: return "foreign";
		default: return "unknown";
	}
}

std::string code_integrity_name(uint32_t value) {
	switch (value) {
		case 0u: return "unknown";
		case 1u: return "disabled";
		case 2u: return "enabled";
		case 3u: return "audit";
		case 4u: return "strict";
		default: return "unknown";
	}
}

std::string backend_name(uint32_t value) {
	return value == 1u ? "intel" : value == 2u ? "amd" : "unknown";
}

std::string hook_state_name(uint32_t value) {
	return value == 1u ? "execView" : value == 0u ? "dataView" : "unknown";
}

bool status_is_helper_incompatible(sao_status_t status,
								 bool typed_response_present) noexcept {
	switch (status) {
		case SAO_RT_IO_ERR_HELPER_HANDSHAKE_FAIL:
		case SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH:
		case SAO_RT_IO_ERR_HELPER_PROTOCOL_MISMATCH:
		case SAO_RT_IO_ERR_HELPER_BOOTSTRAP_FAIL:
			return true;
		default:
			return status == SAO_STATUS_ERR_NOT_IMPLEMENTED &&
				!typed_response_present;
	}
}

bool status_is_helper_unavailable(sao_status_t status) noexcept {
	switch (status) {
		case SAO_STATUS_ERR_NOT_INITIALIZED:
		case SAO_RT_IO_ERR_HELPER_NOT_LAUNCHED:
		case SAO_RT_IO_ERR_HELPER_EXITED:
		case SAO_RT_IO_ERR_HELPER_NOT_FOUND:
		case SAO_RT_IO_ERR_HELPER_START_FAILED:
		case SAO_RT_IO_ERR_HELPER_READY_TIMEOUT:
		case SAO_RT_IO_ERR_HELPER_CRASHED:
		case SAO_RT_IO_ERR_SESSION_NOT_RUNNING:
		case SAO_RT_IO_ERR_SESSION_CLOSED:
		case SAO_RT_IO_ERR_SESSION_QUIESCING:
			return true;
		default:
			return false;
	}
}

bool status_is_lifecycle_failure(sao_status_t status,
								 bool typed_response_present) noexcept {
	return status_is_helper_incompatible(status, typed_response_present) ||
		   status_is_helper_unavailable(status) ||
		   status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED ||
		   status == SAO_RT_IO_ERR_PIPE_BROKEN ||
		   status == SAO_RT_IO_ERR_PIPE_CONNECT_FAILED;
}

bool status_is_transport_failure(sao_status_t status) noexcept {
	switch (status) {
		case SAO_STATUS_ERR_TIMEOUT:
		case SAO_RT_IO_ERR_DEADLINE_EXCEEDED:
		case SAO_RT_IO_ERR_PIPE_TIMEOUT:
		case SAO_RT_IO_ERR_PIPE_POOL_EXHAUSTED:
		case SAO_RT_IO_ERR_PIPE_CANCELLED:
		case SAO_RT_IO_ERR_PIPE_BROKEN:
		case SAO_RT_IO_ERR_PIPE_CONNECT_FAILED:
			return true;
		default:
			return false;
	}
}

bool status_is_vt_unavailable(sao_status_t status,
								 bool typed_response_present) noexcept {
	return status == SAO_STATUS_ERR_CAPABILITY_MISSING ||
		   status == SAO_RT_IO_ERR_DRIVER_NOT_LOADED ||
		   (status == SAO_STATUS_ERR_NOT_IMPLEMENTED && typed_response_present);
}

bool has_recovery(uint32_t flags, sao_status_t status) noexcept {
	return (flags & SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_RECOVERY_REQUIRED) != 0u ||
		   status == SAO_RT_IO_ERR_CLEANUP_ORPHAN_LEFT ||
		   status == SAO_RT_IO_ERR_RESTORE_PARTIAL;
}

bool has_guest_readonly(uint32_t flags) noexcept {
	return (flags & SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_GUEST_READONLY) != 0u;
}

std::string operation_reason(sao_status_t status, uint32_t flags,
							 const CallResult& call, bool mutation,
							 bool typed_response_present = false) {
	if (mutation && (status == SAO_RT_IO_ERR_MUTATION_UNKNOWN ||
					 call.outcome == SAO_RT_IO_WIRE_OUTCOME_UNKNOWN ||
					 (flags & SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN) != 0u)) {
		return "mutation_unknown";
	}
	if (has_recovery(flags, status)) return "admission/recovery_required";
	if (status == SAO_STATUS_ERR_NOT_IMPLEMENTED && typed_response_present)
		return "vt_unavailable";
	if (has_guest_readonly(flags) || status == SAO_RT_IO_ERR_PRIOR_HYPERVISOR)
		return "guest_readonly";
	if (call.outcome == SAO_RT_IO_WIRE_OUTCOME_CANCELLED ||
		status == SAO_STATUS_ERR_CANCELLED ||
		status == SAO_RT_IO_ERR_PIPE_CANCELLED) {
		return "cancelled";
	}
	if (status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED)
		return "session_rekey_required";
	if (status_is_helper_incompatible(status, typed_response_present))
		return "helper_incompatible";
	if (status_is_helper_unavailable(status)) return "helper_unavailable";
	if (status_is_vt_unavailable(status, typed_response_present))
		return "vt_unavailable";
	if (status_is_transport_failure(status)) return "transport_error";
	return "operation_failed";
}

Json operation_failure(sao_status_t status, uint32_t flags,
					   const CallResult& call, bool mutation,
					   bool typed_response_present = false) {
	const bool guest_readonly = has_guest_readonly(flags) ||
		status == SAO_RT_IO_ERR_PRIOR_HYPERVISOR;
	const bool recovery = has_recovery(flags, status);
	const bool unknown = (flags & SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN) != 0u ||
						 (mutation && call.outcome == SAO_RT_IO_WIRE_OUTCOME_UNKNOWN) ||
						 status == SAO_RT_IO_ERR_MUTATION_UNKNOWN;
	const bool partial = (flags & SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_PARTIAL) != 0u;
	return Json{{"ok", false},
				{"available", false},
				{"statusCode", static_cast<int32_t>(status)},
				{"reason", operation_reason(status, flags, call, mutation,
											  typed_response_present)},
				{"guestReadonly", guest_readonly},
				{"recoveryRequired", recovery},
				{"unknown", unknown},
				{"partial", partial}};
}

Json success_result(bool available = true, bool guest_readonly = false,
					bool recovery = false) {
	return Json{{"ok", true},
				{"available", available},
				{"guestReadonly", guest_readonly},
				{"recoveryRequired", recovery},
				{"unknown", false},
				{"partial", false}};
}

bool component_response_present(const SaoRtIoVtProxyResponsePrefix& prefix) noexcept {
	return std::memcmp(prefix.header.magic, SAO_RT_IO_VT_PROXY_RESPONSE_MAGIC, 4u) == 0 &&
		   prefix.header.version == SAO_RT_IO_VT_PROXY_VERSION &&
		   prefix.header.header_size == SAO_RT_IO_VT_PROXY_HEADER_SIZE &&
		   prefix.header.struct_size >= SAO_RT_IO_VT_PROXY_RESPONSE_PREFIX_SIZE &&
		   prefix.header.reserved == 0u &&
		   (prefix.response_flags & ~SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_ALLOWED_MASK) == 0u;
}

Json status_component_json(const SaoRtIoVtProxyStatusResponse& response,
						   sao_status_t status, const CallResult& call) {
	if (!component_response_present(response.prefix)) {
		return operation_failure(status, response.prefix.response_flags, call, false);
	}
	const auto& value = response.status;
	const bool guest_readonly = value.state == kStateGuestReadonly ||
		value.engine_state == kStateGuestReadonly ||
		(value.status_flags & (kStatusExtGuestReadonly | kStatusExtPriorHypervisor)) != 0u ||
		has_guest_readonly(response.prefix.response_flags);
	const bool recovery = has_recovery(response.prefix.response_flags, status) ||
		(value.status_flags & (kStatusExtStale | kStatusExtRemoved |
							   kStatusExtDeadmanRestartFailed)) != 0u;
	const bool available = status == SAO_STATUS_OK &&
		(value.state == kStateActive ||
							value.state == kStateGuestReadonly) &&
						   (value.engine_state == kStateActive ||
							value.engine_state == kStateGuestReadonly) &&
						   !recovery;
	Json result{{"ok", status == SAO_STATUS_OK},
				{"available", available},
				{"statusCode", static_cast<int32_t>(status)},
				{"operationStatus", response.prefix.operation_status},
				{"state", state_name(value.state)},
				{"engineState", state_name(value.engine_state)},
				{"abiVersion", hex_u64(value.abi_version)},
				{"roots", hex_u64(value.root_cpu_count)},
				{"hooks", hex_u64(value.armed_hooks)},
				{"heartbeat", hex_u64(value.heartbeat_age_ms)},
				{"statusFlags", hex_u64(value.status_flags)},
				{"guestReadonly", guest_readonly},
				{"recoveryRequired", recovery},
				{"deadmanRestartFailed",
				 (value.status_flags & kStatusExtDeadmanRestartFailed) != 0u},
				{"deadmanDegraded",
				 (value.status_flags & kStatusExtDeadmanRestartFailed) != 0u || recovery},
				{"tscCompensationUnavailable",
				 (value.status_flags & kStatusExtTscCompensationUnavailable) != 0u},
				{"availability", Json{{"reason", availability_reason_name(value.availability_reason)},
									   {"reasonCode", value.availability_reason}}},
				{"priorHypervisor", Json{{"kind", prior_hypervisor_name(value.prior_hypervisor_kind)},
										  {"kindCode", value.prior_hypervisor_kind},
										  {"signatureHex", hex_bytes(value.prior_hypervisor_signature,
																	 sizeof(value.prior_hypervisor_signature))}}},
				{"codeIntegrity", Json{{"state", code_integrity_name(value.code_integrity_state)},
										{"stateCode", value.code_integrity_state},
										{"options", hex_u64(value.code_integrity_options)}}},
				{"unknown", false},
				{"partial", false}};
	if (status != SAO_STATUS_OK)
		result["reason"] = operation_reason(status, response.prefix.response_flags,
										  call, false, true);
	return result;
}

Json capabilities_component_json(const SaoRtIoVtProxyCapabilitiesResponse& response,
								 sao_status_t status, const CallResult& call) {
	if (!component_response_present(response.prefix)) {
		return operation_failure(status, response.prefix.response_flags, call, false);
	}
	const auto& value = response.capabilities;
	const uint64_t advertised = value.supported_flags | value.active_flags;
	const bool guest_readonly =
		(advertised & SAO_RT_IO_VT_PROXY_CAPABILITY_GUEST_READONLY) != 0u;
	const bool recovery = has_recovery(response.prefix.response_flags, status);
	Json supported = Json::array();
	Json active = Json::array();
	for (size_t index = 0u; index < kCapabilityCount; ++index) {
		const uint64_t bit = 1ull << index;
		if ((value.supported_flags & bit) != 0u) supported.push_back(kCapabilityNames[index]);
		if ((value.active_flags & bit) != 0u) active.push_back(kCapabilityNames[index]);
	}
	Json result{{"ok", status == SAO_STATUS_OK},
				{"available", status == SAO_STATUS_OK && !recovery},
				{"statusCode", static_cast<int32_t>(status)},
				{"operationStatus", response.prefix.operation_status},
				{"supported", std::move(supported)},
				{"active", std::move(active)},
				{"vendor", hex_u64(value.vendor)},
				{"cpuCount", hex_u64(value.cpu_count)},
				{"maxCpuCount", hex_u64(value.max_cpu_count)},
				{"rawVmxEptVpidCap", hex_u64(value.raw_vmx_ept_vpid_cap)},
				{"rawSvmFeatures", hex_u64(value.raw_svm_features)},
				{"guestReadonly", guest_readonly},
				{"recoveryRequired", recovery},
				{"unknown", false},
				{"partial", false}};
	if (status != SAO_STATUS_OK)
		result["reason"] = operation_reason(status, response.prefix.response_flags,
										  call, false, true);
	return result;
}

Json hook_row_json(const SaoRtIoVtProxyHookRow& row) {
	return Json{{"hookId", hex_u64(row.hook_id)},
				{"gpa", hex_u64(row.gpa)},
				{"writeCount", hex_u64(row.write_count)},
				{"backend", backend_name(row.backend)},
				{"state", hook_state_name(row.state)},
				{"stateCode", row.state},
				{"installed", true},
				{"hidden", row.hook_flags == 1u},
				{"patchSize", row.patch_size}};
}

Json list_failure(sao_status_t status, const SaoRtIoVtProxyListResponse& response,
				  const CallResult& call) {
	Json result = operation_failure(
		status, response.prefix.response_flags, call, false,
		component_response_present(response.prefix));
	if (component_response_present(response.prefix))
		result["operationStatus"] = response.prefix.operation_status;
	return result;
}

Json probe_component_json(const SaoRtIoVtProxyProbeResponse& response,
						  sao_status_t status, const CallResult& call) {
	if (status != SAO_STATUS_OK || !component_response_present(response.prefix)) {
		Json result = operation_failure(
			status, response.prefix.response_flags, call, false,
			component_response_present(response.prefix));
		if (component_response_present(response.prefix))
			result["operationStatus"] = response.prefix.operation_status;
		return result;
	}
	Json items = Json::object();
	for (size_t index = 0u; index < kProbeItemNames.size(); ++index) {
		const bool requested = (response.requested_mask & kProbeItemBits[index]) != 0u;
		const bool ready = (response.ready_mask & kProbeItemBits[index]) != 0u;
		Json item{{"requested", requested},
				  {"ready", ready},
				  {"statusCode", response.item_status[index]}};
		if (response.item_status[index] != SAO_STATUS_OK) {
			CallResult empty_call{};
			item["reason"] = operation_reason(response.item_status[index],
												response.prefix.response_flags,
															empty_call, false, true);
		}
		items[std::string(kProbeItemNames[index])] = std::move(item);
	}
	const bool guest_readonly = has_guest_readonly(response.prefix.response_flags);
	const bool recovery = has_recovery(response.prefix.response_flags, status);
	return Json{{"ok", response.ready_mask == response.requested_mask},
				{"available", response.ready_mask == response.requested_mask &&
														!recovery},
				{"statusCode", static_cast<int32_t>(status)},
				{"operationStatus", response.prefix.operation_status},
				{"requested", hex_u64(response.requested_mask)},
				{"completed", hex_u64(response.completed_mask)},
				{"ready", hex_u64(response.ready_mask)},
				{"items", std::move(items)},
				{"guestReadonly", guest_readonly},
				{"recoveryRequired", recovery},
				{"unknown", false},
				{"partial", false}};
}

}  // namespace

struct Bridge::Impl final {
	struct FenceResult final {
		sao_status_t status = SAO_STATUS_OK;
		bool transitioned = false;
		bool failed = false;
	};

	std::mutex mutex;
	RuntimeHandle runtime = nullptr;
	uint64_t epoch = 0u;
	bool epoch_valid = false;

	bool status_cache_valid = false;
	Clock::time_point status_cache_time{};
	uint64_t status_cache_epoch = 0u;
	SaoRtIoVtProxyStatusResponse status_response{};
	CallResult status_call{};
	sao_status_t status_code = SAO_STATUS_OK;

	bool capabilities_cache_valid = false;
	Clock::time_point capabilities_cache_time{};
	uint64_t capabilities_cache_epoch = 0u;
	SaoRtIoVtProxyCapabilitiesResponse capabilities_response{};
	CallResult capabilities_call{};
	sao_status_t capabilities_code = SAO_STATUS_OK;

	bool aggregate_cache_valid = false;
	Clock::time_point aggregate_cache_time{};
	uint64_t aggregate_cache_epoch = 0u;
	Json aggregate_cache = Json::object();

	bool perf_sample_valid = false;
	uint64_t perf_sample_total = 0u;
	Clock::time_point perf_sample_time{};
	uint64_t perf_sample_epoch = 0u;

	sao_status_t ensure_runtime_locked();
	sao_status_t sync_epoch_locked();
	void invalidate_cache_locked(bool reset_perf);
	FenceResult fence_call_locked(sao_status_t status, uint64_t call_epoch,
									 bool typed_response_present);
	bool fresh(Clock::time_point timestamp, uint64_t cache_epoch) const noexcept;
	sao_status_t query_status_locked();
	sao_status_t query_capabilities_locked();
	sao_status_t query_probe_locked(uint64_t items_mask,
									SaoRtIoVtProxyProbeResponse& response,
									CallResult& call);
	sao_status_t query_hooks_locked(
		std::array<SaoRtIoVtProxyHookRow, SAO_RT_IO_VT_PROXY_MAX_HOOKS>& rows,
		size_t& count, SaoRtIoVtProxyListResponse& response, CallResult& call);
	sao_status_t query_perf_locked(SaoRtIoVtProxyPerfResponse& response,
								   CallResult& call);
	bool mutation_preflight_locked(std::string_view tool_name, Json& result);
	Json status_json_locked();
	int32_t execute_locked(std::string_view tool_name, const Json& arguments,
						   Json& result);
};

bool Bridge::Impl::fresh(Clock::time_point timestamp,
						 uint64_t cache_epoch) const noexcept {
	return epoch_valid && cache_epoch == epoch &&
		   timestamp != Clock::time_point{} &&
		   Clock::now() - timestamp < kCacheLifetime;
}

void Bridge::Impl::invalidate_cache_locked(bool reset_perf) {
	status_cache_valid = false;
	status_cache_time = Clock::time_point{};
	status_cache_epoch = 0u;
	capabilities_cache_valid = false;
	capabilities_cache_time = Clock::time_point{};
	capabilities_cache_epoch = 0u;
	aggregate_cache_valid = false;
	aggregate_cache_epoch = 0u;
	aggregate_cache = Json::object();
	if (reset_perf) {
		perf_sample_valid = false;
		perf_sample_total = 0u;
		perf_sample_time = Clock::time_point{};
		perf_sample_epoch = 0u;
	}
}

sao_status_t Bridge::Impl::ensure_runtime_locked() {
	if (runtime != nullptr) return SAO_STATUS_OK;
	SaoRtIoProxyRuntimeConfig config{};
	config.autospawn = 0u;
	RuntimeHandle created = nullptr;
	const sao_status_t status = sao_rt_io_proxy_runtime_create(&config, &created);
	if (status != SAO_STATUS_OK || created == nullptr) {
		invalidate_cache_locked(true);
		return status == SAO_STATUS_OK ? SAO_RT_IO_ERR_INTERNAL_ERROR : status;
	}
	runtime = created;
	epoch_valid = false;
	invalidate_cache_locked(true);
	return SAO_STATUS_OK;
}

sao_status_t Bridge::Impl::sync_epoch_locked() {
	const sao_status_t runtime_status = ensure_runtime_locked();
	if (runtime_status != SAO_STATUS_OK) return runtime_status;
	SaoRtIoProxyRuntimeSnapshot snapshot{};
	const sao_status_t status = sao_rt_io_proxy_runtime_snapshot(runtime, &snapshot);
	if (status != SAO_STATUS_OK) {
		epoch_valid = false;
		invalidate_cache_locked(true);
		return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
	}
	if (!epoch_valid || epoch != snapshot.epoch) {
		epoch = snapshot.epoch;
		epoch_valid = true;
		invalidate_cache_locked(true);
	}
	return SAO_STATUS_OK;
}

Bridge::Impl::FenceResult Bridge::Impl::fence_call_locked(
	sao_status_t status, uint64_t call_epoch,
	bool typed_response_present) {
	if (status_is_lifecycle_failure(status, typed_response_present)) {
		invalidate_cache_locked(true);
	}
	SaoRtIoProxyRuntimeSnapshot snapshot{};
	const sao_status_t snapshot_status =
		sao_rt_io_proxy_runtime_snapshot(runtime, &snapshot);
	if (snapshot_status != SAO_STATUS_OK) {
		epoch_valid = false;
		invalidate_cache_locked(true);
		return FenceResult{SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, false, true};
	}
	if (!epoch_valid || epoch != snapshot.epoch || call_epoch != snapshot.epoch) {
		epoch = snapshot.epoch;
		epoch_valid = true;
		invalidate_cache_locked(true);
		return FenceResult{status, true, false};
	}
	return FenceResult{status, false, false};
}

sao_status_t Bridge::Impl::query_status_locked() {
	const sao_status_t preflight = sync_epoch_locked();
	if (preflight != SAO_STATUS_OK) {
		status_cache_valid = false;
		status_cache_epoch = 0u;
		status_response = SaoRtIoVtProxyStatusResponse{};
		status_call = CallResult{};
		status_code = preflight;
		return preflight;
	}
	if (status_cache_valid &&
		fresh(status_cache_time, status_cache_epoch))
		return status_code;
	for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
		SaoRtIoVtProxyStatusResponse response{};
		CallResult call{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_status_v2(
			runtime, kTimeoutMs, &response, &call);
		const FenceResult fence = fence_call_locked(
			status, call_epoch, component_response_present(response.prefix));
		if (fence.failed || fence.transitioned && attempt != 0u) {
			status_response = SaoRtIoVtProxyStatusResponse{};
			status_call = CallResult{};
			status_code = SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
			status_cache_valid = false;
			status_cache_epoch = 0u;
			status_cache_time = Clock::time_point{};
			return status_code;
		}
		if (fence.transitioned) continue;
		status_response = response;
		status_call = call;
		status_code = status;
		status_cache_valid = !status_is_lifecycle_failure(
			status, component_response_present(response.prefix));
		status_cache_epoch = status_cache_valid ? epoch : 0u;
		status_cache_time = status_cache_valid ? Clock::now()
																  : Clock::time_point{};
		return status;
	}
	return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
}

sao_status_t Bridge::Impl::query_capabilities_locked() {
	const sao_status_t preflight = sync_epoch_locked();
	if (preflight != SAO_STATUS_OK) {
		capabilities_cache_valid = false;
		capabilities_cache_epoch = 0u;
		capabilities_response = SaoRtIoVtProxyCapabilitiesResponse{};
		capabilities_call = CallResult{};
		capabilities_code = preflight;
		return preflight;
	}
	if (capabilities_cache_valid &&
		fresh(capabilities_cache_time, capabilities_cache_epoch)) {
		return capabilities_code;
	}
	for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
		SaoRtIoVtProxyCapabilitiesResponse response{};
		CallResult call{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_capabilities(
			runtime, kTimeoutMs, &response, &call);
		const FenceResult fence = fence_call_locked(
			status, call_epoch, component_response_present(response.prefix));
		if (fence.failed || fence.transitioned && attempt != 0u) {
			capabilities_response = SaoRtIoVtProxyCapabilitiesResponse{};
			capabilities_call = CallResult{};
			capabilities_code = SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
			capabilities_cache_valid = false;
			capabilities_cache_epoch = 0u;
			capabilities_cache_time = Clock::time_point{};
			return capabilities_code;
		}
		if (fence.transitioned) continue;
		capabilities_response = response;
		capabilities_call = call;
		capabilities_code = status;
		capabilities_cache_valid = !status_is_lifecycle_failure(
			status, component_response_present(response.prefix));
		capabilities_cache_epoch = capabilities_cache_valid ? epoch : 0u;
		capabilities_cache_time = capabilities_cache_valid ? Clock::now()
																		 : Clock::time_point{};
		return status;
	}
	return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
}

sao_status_t Bridge::Impl::query_probe_locked(
	uint64_t items_mask, SaoRtIoVtProxyProbeResponse& response,
	CallResult& call) {
	response = SaoRtIoVtProxyProbeResponse{};
	call = CallResult{};
	const sao_status_t preflight = sync_epoch_locked();
	if (preflight != SAO_STATUS_OK) return preflight;
	for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
		response = SaoRtIoVtProxyProbeResponse{};
		call = CallResult{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_probe(
			runtime, items_mask, kTimeoutMs, &response, &call);
		const FenceResult fence = fence_call_locked(
			status, call_epoch, component_response_present(response.prefix));
		if (fence.failed || fence.transitioned && attempt != 0u) {
			response = SaoRtIoVtProxyProbeResponse{};
			call = CallResult{};
			return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
		}
		if (fence.transitioned) continue;
		return status;
	}
	return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
}

sao_status_t Bridge::Impl::query_hooks_locked(
	std::array<SaoRtIoVtProxyHookRow, SAO_RT_IO_VT_PROXY_MAX_HOOKS>& rows,
	size_t& count, SaoRtIoVtProxyListResponse& response, CallResult& call) {
	rows.fill(SaoRtIoVtProxyHookRow{});
	count = 0u;
	response = SaoRtIoVtProxyListResponse{};
	call = CallResult{};
	const sao_status_t preflight = sync_epoch_locked();
	if (preflight != SAO_STATUS_OK) return preflight;
	for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
		rows.fill(SaoRtIoVtProxyHookRow{});
		count = 0u;
		response = SaoRtIoVtProxyListResponse{};
		call = CallResult{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_list_hooks(
			runtime, SAO_RT_IO_VT_PROXY_LIST_FILTER_NONE, rows.data(), rows.size(),
			&count, kTimeoutMs, &response, &call);
		const FenceResult fence = fence_call_locked(
			status, call_epoch, component_response_present(response.prefix));
		if (fence.failed || fence.transitioned && attempt != 0u) {
			rows.fill(SaoRtIoVtProxyHookRow{});
			count = 0u;
			response = SaoRtIoVtProxyListResponse{};
			call = CallResult{};
			return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
		}
		if (fence.transitioned) continue;
		return status;
	}
	return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
}

sao_status_t Bridge::Impl::query_perf_locked(
	SaoRtIoVtProxyPerfResponse& response, CallResult& call) {
	response = SaoRtIoVtProxyPerfResponse{};
	call = CallResult{};
	const sao_status_t preflight = sync_epoch_locked();
	if (preflight != SAO_STATUS_OK) return preflight;
	for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
		response = SaoRtIoVtProxyPerfResponse{};
		call = CallResult{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_perf_stats(
			runtime, kTimeoutMs, &response, &call);
		const FenceResult fence = fence_call_locked(
			status, call_epoch, component_response_present(response.prefix));
		if (fence.failed || fence.transitioned && attempt != 0u) {
			response = SaoRtIoVtProxyPerfResponse{};
			call = CallResult{};
			return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
		}
		if (fence.transitioned) continue;
		return status;
	}
	return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
}


bool Bridge::Impl::mutation_preflight_locked(std::string_view tool_name,
											 Json& result) {
	const sao_status_t epoch_status = sync_epoch_locked();
	if (epoch_status != SAO_STATUS_OK) {
		result = operation_failure(epoch_status, 0u, CallResult{}, false);
		result["operation"] = tool_name;
		result["dispatched"] = false;
		return false;
	}
	const uint64_t preflight_epoch = epoch;
	const sao_status_t status_code_local = query_status_locked();
	const sao_status_t capabilities_status = query_capabilities_locked();
	if (status_code_local == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED ||
		capabilities_status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED) {
		result = operation_failure(SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, 0u,
										 CallResult{}, false);
		result["operation"] = tool_name;
		result["dispatched"] = false;
		return false;
	}
	if (status_code_local != SAO_STATUS_OK ||
		!component_response_present(status_response.prefix)) {
		result = status_component_json(status_response, status_code_local, status_call);
		result["operation"] = tool_name;
		result["dispatched"] = false;
		return false;
	}

	const auto& status = status_response.status;
	const bool status_guest_readonly =
		status.state == kStateGuestReadonly ||
		status.engine_state == kStateGuestReadonly ||
		(status.status_flags & (kStatusExtGuestReadonly | kStatusExtPriorHypervisor)) != 0u ||
		has_guest_readonly(status_response.prefix.response_flags);
	const bool status_recovery =
		has_recovery(status_response.prefix.response_flags, status_code_local) ||
		(status.status_flags & (kStatusExtStale | kStatusExtRemoved |
								kStatusExtDeadmanRestartFailed)) != 0u;
	const bool capabilities_guest_readonly =
		capabilities_status == SAO_STATUS_OK &&
		component_response_present(capabilities_response.prefix) &&
		((capabilities_response.capabilities.supported_flags |
		  capabilities_response.capabilities.active_flags) &
		 SAO_RT_IO_VT_PROXY_CAPABILITY_GUEST_READONLY) != 0u;
	if (capabilities_status != SAO_STATUS_OK ||
		!component_response_present(capabilities_response.prefix)) {
		result = capabilities_component_json(capabilities_response,
											 capabilities_status, capabilities_call);
		result["operation"] = tool_name;
		result["dispatched"] = false;
		return false;
	}

	SaoRtIoProxyRuntimeSnapshot snapshot{};
	const sao_status_t snapshot_status =
		sao_rt_io_proxy_runtime_snapshot(runtime, &snapshot);
	if (snapshot_status != SAO_STATUS_OK) {
		epoch_valid = false;
		invalidate_cache_locked(true);
		result = operation_failure(SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, 0u,
										 CallResult{}, false);
		result["operation"] = tool_name;
		result["dispatched"] = false;
		return false;
	}
	if (!epoch_valid || snapshot.epoch != preflight_epoch ||
		!status_cache_valid || !capabilities_cache_valid ||
		status_cache_epoch != snapshot.epoch ||
		capabilities_cache_epoch != snapshot.epoch) {
		epoch = snapshot.epoch;
		epoch_valid = true;
		invalidate_cache_locked(true);
		result = operation_failure(SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, 0u,
										 CallResult{}, false);
		result["operation"] = tool_name;
		result["dispatched"] = false;
		return false;
	}

	const bool guest_readonly = status_guest_readonly || capabilities_guest_readonly;
	const bool recovery = status_recovery;
	const bool active = status.state == kStateActive &&
						 status.engine_state == kStateActive;
	if (!active || guest_readonly || recovery) {
		const std::string reason = guest_readonly
										   ? "guest_readonly"
										   : recovery
												 ? "admission/recovery_required"
												 : "vt_unavailable";
		result = Json{{"ok", false},
					  {"available", false},
					  {"reason", reason},
					  {"operation", tool_name},
					  {"dispatched", false},
					  {"guestReadonly", guest_readonly},
					  {"recoveryRequired", recovery},
					  {"unknown", false},
					  {"partial", false},
					  {"state", state_name(status.state)},
					  {"engineState", state_name(status.engine_state)}};
		return false;
	}
	return true;
}

Json Bridge::Impl::status_json_locked() {
	for (uint32_t aggregate_attempt = 0u; aggregate_attempt < 2u;
		 aggregate_attempt++) {
		const sao_status_t cache_status = sync_epoch_locked();
		if (cache_status != SAO_STATUS_OK) {
			return operation_failure(cache_status, 0u, CallResult{}, false);
		}
		if (aggregate_cache_valid &&
			fresh(aggregate_cache_time, aggregate_cache_epoch)) {
			return aggregate_cache;
		}
		const uint64_t start_epoch = epoch;

		const sao_status_t status_code_local = query_status_locked();
		if (status_code_local == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED)
			return operation_failure(status_code_local, 0u, CallResult{}, false);
		const sao_status_t capabilities_status = query_capabilities_locked();
		if (capabilities_status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED)
			return operation_failure(capabilities_status, 0u, CallResult{}, false);
		SaoRtIoVtProxyProbeResponse probe_response{};
		CallResult probe_call{};
		const sao_status_t probe_status = query_probe_locked(
			SAO_RT_IO_VT_PROXY_PROBE_ITEM_ALL, probe_response, probe_call);
		if (probe_status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED)
			return operation_failure(probe_status, 0u, CallResult{}, false);
		std::array<SaoRtIoVtProxyHookRow, SAO_RT_IO_VT_PROXY_MAX_HOOKS> hook_rows{};
		size_t hook_count = 0u;
		SaoRtIoVtProxyListResponse list_response{};
		CallResult list_call{};
		const sao_status_t list_status = query_hooks_locked(
			hook_rows, hook_count, list_response, list_call);
		if (list_status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED)
			return operation_failure(list_status, 0u, CallResult{}, false);
		SaoRtIoVtProxyPerfResponse perf_response{};
		CallResult perf_call{};
		const sao_status_t perf_status = query_perf_locked(perf_response, perf_call);
		if (perf_status == SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED)
			return operation_failure(perf_status, 0u, CallResult{}, false);

		const bool status_present = component_response_present(status_response.prefix);
		const bool capabilities_present =
			component_response_present(capabilities_response.prefix);
		const bool list_present = component_response_present(list_response.prefix);
		const bool probe_present = component_response_present(probe_response.prefix);
		const bool perf_present = component_response_present(perf_response.prefix);
		const bool status_valid = status_code_local == SAO_STATUS_OK && status_present;
		const bool capabilities_valid =
			capabilities_status == SAO_STATUS_OK && capabilities_present;
		const bool list_valid = list_status == SAO_STATUS_OK && list_present &&
								 hook_count <= hook_rows.size();
		const bool probe_valid = probe_status == SAO_STATUS_OK && probe_present &&
								  probe_response.ready_mask == probe_response.requested_mask;
		const bool perf_valid = perf_status == SAO_STATUS_OK && perf_present;
		const bool all_ok = status_valid && capabilities_valid && probe_valid &&
							list_valid && perf_valid;

		Json hook_list;
		if (list_valid) {
			Json hooks = Json::array();
			Json hidden_hooks = Json::array();
			for (size_t index = 0u; index < hook_count; ++index) {
				Json row = hook_row_json(hook_rows[index]);
				if (hook_rows[index].hook_flags == 1u) hidden_hooks.push_back(row);
				hooks.push_back(std::move(row));
			}
			hook_list = Json{{"hookList", std::move(hooks)},
							 {"hiddenHooks", std::move(hidden_hooks)}};
			hook_list["hiddenHookCount"] =
				hex_u64(hook_list["hiddenHooks"].size());
		} else {
			hook_list = Json{{"hookList", list_failure(list_status,
																		 list_response, list_call)}};
		}

		Json perf;
		if (perf_valid) {
			uint64_t total = 0u;
			for (const uint64_t value : perf_response.stats.vmexit_count) {
				if (std::numeric_limits<uint64_t>::max() - total < value) {
					total = std::numeric_limits<uint64_t>::max();
					break;
				}
				total += value;
			}
			const auto now = Clock::now();
			bool sampled = false;
			double rate = 0.0;
			if (perf_sample_valid && perf_sample_epoch == epoch &&
				total >= perf_sample_total && now > perf_sample_time) {
				const double seconds =
					std::chrono::duration<double>(now - perf_sample_time).count();
				if (seconds > 0.0) {
					sampled = true;
					rate = static_cast<double>(total - perf_sample_total) / seconds;
				}
			}
			const bool recovery = has_recovery(
				perf_response.prefix.response_flags, perf_status);
			perf_sample_valid = true;
			perf_sample_total = total;
			perf_sample_time = now;
			perf_sample_epoch = epoch;
			perf = Json{{"ok", true},
						{"available", !recovery},
						{"cpuCount", hex_u64(perf_response.stats.cpu_count)},
						{"vmexitCumulative", hex_u64(total)},
						{"vmexitRatePerSec", rate},
						{"sampled", sampled},
						{"operationStatus", perf_response.prefix.operation_status},
						{"guestReadonly", has_guest_readonly(
							perf_response.prefix.response_flags)},
						{"recoveryRequired", recovery},
						{"unknown", false},
						{"partial", false}};
		} else {
			perf = operation_failure(perf_status,
									 perf_response.prefix.response_flags, perf_call, false,
									 perf_present);
			if (perf_present)
				perf["operationStatus"] = perf_response.prefix.operation_status;
		}

		SaoRtIoProxyRuntimeSnapshot end_snapshot{};
		const sao_status_t end_status =
			sao_rt_io_proxy_runtime_snapshot(runtime, &end_snapshot);
		if (end_status != SAO_STATUS_OK) {
			epoch_valid = false;
			invalidate_cache_locked(true);
			return operation_failure(SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, 0u,
										 CallResult{}, false);
		}
		if (end_snapshot.epoch != start_epoch) {
			epoch = end_snapshot.epoch;
			epoch_valid = true;
			invalidate_cache_locked(true);
			if (aggregate_attempt == 0u) continue;
			return operation_failure(SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, 0u,
										 CallResult{}, false);
		}

		Json result{{"ok", all_ok},
					{"available", false},
					{"guestReadonly", false},
					{"recoveryRequired", false},
					{"deadmanRestartFailed", false},
					{"deadmanDegraded", false},
					{"unknown", false},
					{"partial", false},
					{"status", status_component_json(status_response,
																	status_code_local, status_call)},
					{"capabilities", capabilities_component_json(
																				 capabilities_response,
																				 capabilities_status,
																				 capabilities_call)},
					{"probe", probe_component_json(probe_response, probe_status,
																			 probe_call)},
					{"perf", std::move(perf)},
					{"hookList", hook_list.value("hookList", Json::object())}};
		if (hook_list.contains("hiddenHooks")) {
			result["hiddenHooks"] = hook_list["hiddenHooks"];
			result["hiddenHookCount"] = hook_list["hiddenHookCount"];
		}

		bool top_guest_readonly = false;
		bool top_recovery = false;
		bool top_unknown = false;
		bool top_partial = false;
		bool top_deadman_restart_failed = false;
		bool top_deadman_degraded = false;
		std::string failure_reason;
		const auto merge_component = [&](const Json& component) {
			if (!component.is_object()) return;
			top_guest_readonly = top_guest_readonly ||
				component.value("guestReadonly", false);
			top_recovery = top_recovery ||
				component.value("recoveryRequired", false);
			top_unknown = top_unknown || component.value("unknown", false);
			top_partial = top_partial || component.value("partial", false);
			top_deadman_restart_failed = top_deadman_restart_failed ||
				component.value("deadmanRestartFailed", false);
			top_deadman_degraded = top_deadman_degraded ||
				component.value("deadmanDegraded", false);
			if (!component.value("ok", false) && failure_reason.empty() &&
				component.contains("reason") && component["reason"].is_string()) {
				failure_reason = component["reason"].get<std::string>();
			}
		};
		merge_component(result["status"]);
		merge_component(result["capabilities"]);
		merge_component(result["probe"]);
		merge_component(result["perf"]);
		merge_component(result["hookList"]);

		bool status_runtime_available = false;
		if (status_present) {
			const auto& value = status_response.status;
			status_runtime_available = status_code_local == SAO_STATUS_OK &&
				(value.state == kStateActive || value.state == kStateGuestReadonly) &&
				(value.engine_state == kStateActive ||
				 value.engine_state == kStateGuestReadonly);
			result["state"] = state_name(value.state);
			result["engineState"] = state_name(value.engine_state);
			result["abiVersion"] = hex_u64(value.abi_version);
			result["roots"] = hex_u64(value.root_cpu_count);
			result["hooks"] = hex_u64(value.armed_hooks);
			result["heartbeat"] = hex_u64(value.heartbeat_age_ms);
			result["availability"] = Json{
				{"reason", availability_reason_name(value.availability_reason)},
				{"reasonCode", value.availability_reason}};
			result["priorHypervisor"] = Json{
				{"kind", prior_hypervisor_name(value.prior_hypervisor_kind)},
				{"kindCode", value.prior_hypervisor_kind},
				{"signatureHex", hex_bytes(value.prior_hypervisor_signature,
																			 sizeof(value.prior_hypervisor_signature))}};
			result["codeIntegrity"] = Json{
				{"state", code_integrity_name(value.code_integrity_state)},
				{"stateCode", value.code_integrity_state},
				{"options", hex_u64(value.code_integrity_options)}};
		} else {
			result["statusCode"] = static_cast<int32_t>(status_code_local);
		}

		top_deadman_degraded = top_deadman_degraded || top_recovery;
		const bool available = all_ok && status_runtime_available && !top_recovery;
		result["available"] = available;
		result["guestReadonly"] = top_guest_readonly;
		result["recoveryRequired"] = top_recovery;
		result["deadmanRestartFailed"] = top_deadman_restart_failed;
		result["deadmanDegraded"] = top_deadman_degraded;
		result["unknown"] = top_unknown;
		result["partial"] = top_partial;
		if (!available) {
			if (top_recovery) {
				result["reason"] = "admission/recovery_required";
			} else if (!failure_reason.empty()) {
				result["reason"] = failure_reason;
			} else if (top_guest_readonly && !status_runtime_available) {
				result["reason"] = "guest_readonly";
			} else if (!status_runtime_available) {
				result["reason"] = "vt_unavailable";
			} else {
				result["reason"] = "operation_failed";
			}
		}

		aggregate_cache = result;
		aggregate_cache_valid =
			!status_is_lifecycle_failure(status_code_local, status_present) &&
			!status_is_lifecycle_failure(capabilities_status, capabilities_present) &&
			!status_is_lifecycle_failure(probe_status, probe_present) &&
			!status_is_lifecycle_failure(list_status, list_present) &&
			!status_is_lifecycle_failure(perf_status, perf_present);
		aggregate_cache_epoch = aggregate_cache_valid ? epoch : 0u;
		aggregate_cache_time = aggregate_cache_valid ? Clock::now()
																		 : Clock::time_point{};
		return result;
	}
	return operation_failure(SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED, 0u,
								 CallResult{}, false);
}

int32_t Bridge::Impl::execute_locked(std::string_view tool_name,
									 const Json& arguments, Json& result) {
	Json error;
	if (tool_name == "vt.status") {
		if (!exact_fields(arguments, {}, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		result = status_json_locked();
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.capabilities") {
		if (!exact_fields(arguments, {}, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		const sao_status_t status = query_capabilities_locked();
		result = capabilities_component_json(capabilities_response, status,
											 capabilities_call);
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.probe") {
		uint64_t items_mask = 0u;
		if (!exact_fields(arguments, {"items"}, error) ||
			!parse_items_mask(arguments, items_mask, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		SaoRtIoVtProxyProbeResponse response{};
		CallResult call{};
		const sao_status_t status = query_probe_locked(items_mask, response, call);
		result = probe_component_json(response, status, call);
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.hookPage") {
		if (!exact_fields(arguments, {"gva", "patchBytes", "confirmed"}, error) ||
			!confirmed_true(arguments, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t gva = 0u;
		std::vector<uint8_t> patch;
		const bool parsed_gva = parse_hex_u64(arguments, "gva", gva, error);
		if (parsed_gva && !upper_canonical_gva(gva)) {
			error = Json{{"error", "gva must be a canonical upper-half kernel address"},
						 {"path", "$.gva"}};
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		const bool parsed_patch = parsed_gva &&
			parse_hex_bytes(arguments, "patchBytes", SAO_RT_IO_VT_PROXY_MAX_PATCH,
							patch, error);
		SaoRtIoSecretScope patch_wipe(patch.data(), patch.size());
		if (!parsed_gva || !parsed_patch || patch.size() < 1u ||
			patch.size() > SAO_RT_IO_VT_PROXY_MAX_PATCH) {
			if (error.is_null() || !error.contains("path")) {
				error = Json{{"error", "patchBytes must decode to 1..32 bytes"},
							 {"path", "$.patchBytes"}};
			}
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t end = 0u;
		if (!checked_inclusive_end(gva, patch.size(), end) ||
			!upper_canonical_gva(end) ||
			(gva & 0xfffu) + patch.size() > 0x1000u) {
			result = Json{{"error", "patchBytes must remain within one canonical page"},
						  {"path", "$.gva"}};
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		if (!mutation_preflight_locked(tool_name, result)) return SAO_AI_EDITOR_OK;
		SaoRtIoVtProxyHookPageResponse response{};
		CallResult call{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_hook_page(
			runtime, gva, patch.data(), patch.size(), kTimeoutMs, &response, &call);
		const bool typed = component_response_present(response.prefix);
		const FenceResult fence = fence_call_locked(status, call_epoch, typed);
		const bool fence_unknown = fence.failed || fence.transitioned;
		const bool partial_evidence = typed && hook_id_valid(response.hook_id) &&
			response.gpa != 0u;
		const uint32_t flags = response.prefix.response_flags |
			(fence_unknown ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN : 0u) |
			(fence_unknown && partial_evidence
				 ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_PARTIAL
				 : 0u);
		const sao_status_t effective_status = fence_unknown
			? SAO_RT_IO_ERR_MUTATION_UNKNOWN
			: status;
		invalidate_cache_locked(status != SAO_STATUS_OK || fence_unknown ||
								(flags & (SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN |
											  SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_RECOVERY_REQUIRED)) != 0u);
		result = effective_status == SAO_STATUS_OK
					 ? success_result()
					 : operation_failure(effective_status, flags, call, true, typed);
		if (response.hook_id != 0u) result["hookId"] = hex_u64(response.hook_id);
		if (response.gpa != 0u) result["gpa"] = hex_u64(response.gpa);
		if (typed) result["operationStatus"] = response.prefix.operation_status;
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.hideRegion") {
		if (!exact_fields(arguments,
						  {"gva", "pageCount", "decoyMode", "templateBytes", "confirmed"},
						  error) || !confirmed_true(arguments, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t gva = 0u;
		uint64_t page_count = 0u;
		if (!parse_hex_u64(arguments, "gva", gva, error) ||
			!parse_integer_range(arguments, "pageCount", 1u, 64u,
								 page_count, error) ||
			!arguments.contains("decoyMode") ||
			!arguments["decoyMode"].is_string()) {
			if (!error.contains("path")) {
				error = Json{{"error", "invalid hideRegion argument"},
							 {"path", "$.decoyMode"}};
			}
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t span = 0u;
		uint64_t end = 0u;
		if (!upper_canonical_gva(gva) || (gva & 0xfffu) != 0u ||
			page_count > std::numeric_limits<uint64_t>::max() / 0x1000u ||
			(span = page_count * 0x1000u) == 0u ||
			!checked_inclusive_end(gva, span, end) ||
			!upper_canonical_gva(end)) {
			result = Json{{"error", "gva and pageCount must describe a canonical page-aligned span"},
						  {"path", "$.gva"}};
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		const std::string decoy_mode = arguments["decoyMode"].get<std::string>();
		uint32_t wire_mode = SAO_RT_IO_VT_PROXY_DECOY_ZERO;
		if (decoy_mode == "template") {
			wire_mode = SAO_RT_IO_VT_PROXY_DECOY_TEMPLATE;
		} else if (decoy_mode == "ucs") {
			wire_mode = SAO_RT_IO_VT_PROXY_DECOY_CLEAN_SNAPSHOT;
		} else if (decoy_mode != "zero") {
			result = Json{{"error", "decoyMode must be zero, template, or ucs"},
						  {"path", "$.decoyMode"}};
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		std::vector<uint8_t> template_bytes;
		if (arguments.contains("templateBytes")) {
			if (decoy_mode != "template") {
				result = Json{{"error", "templateBytes is only valid for template decoyMode"},
							  {"path", "$.templateBytes"}};
				return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
			}
			if (!parse_hex_bytes(arguments, "templateBytes",
								 SAO_RT_IO_VT_PROXY_MAX_TEMPLATE,
								 template_bytes, error) ||
					template_bytes.size() < 1u ||
					template_bytes.size() > SAO_RT_IO_VT_PROXY_MAX_TEMPLATE) {
				if (!error.contains("path")) {
					error = Json{{"error", "templateBytes must decode to 1..4096 bytes"},
								 {"path", "$.templateBytes"}};
				}
				result = std::move(error);
				return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
			}
		} else if (decoy_mode == "template") {
			result = Json{{"error", "template decoyMode requires templateBytes"},
						  {"path", "$.templateBytes"}};
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		SaoRtIoSecretScope template_wipe(template_bytes.data(), template_bytes.size());
		if (!mutation_preflight_locked(tool_name, result)) return SAO_AI_EDITOR_OK;
		SaoRtIoVtProxyHideRegionResponse response{};
		CallResult call{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_hide_region(
			runtime, gva, static_cast<uint32_t>(page_count), wire_mode,
			template_bytes.empty() ? nullptr : template_bytes.data(),
			template_bytes.size(), kTimeoutMs, &response, &call);
		const bool typed = component_response_present(response.prefix);
		const FenceResult fence = fence_call_locked(status, call_epoch, typed);
		const bool fence_unknown = fence.failed || fence.transitioned;
		bool partial_evidence = false;
		if (typed) {
			for (uint32_t index = 0u;
				 index < static_cast<uint32_t>(page_count); ++index) {
				if (response.hook_ids[index] != 0u) {
					partial_evidence = true;
					break;
				}
			}
		}
		const uint32_t flags = response.prefix.response_flags |
			(fence_unknown ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN : 0u) |
			(fence_unknown && partial_evidence
				 ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_PARTIAL
				 : 0u);
		const sao_status_t effective_status = fence_unknown
			? SAO_RT_IO_ERR_MUTATION_UNKNOWN
			: status;
		invalidate_cache_locked(status != SAO_STATUS_OK || fence_unknown ||
								(flags & (SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN |
											  SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_RECOVERY_REQUIRED)) != 0u);
		result = effective_status == SAO_STATUS_OK
					 ? success_result()
					 : operation_failure(effective_status, flags, call, true, typed);
		result["installedCount"] = response.installed_count;
		if (typed) result["operationStatus"] = response.prefix.operation_status;
		Json sparse = Json::array();
		for (uint32_t index = 0u; index < static_cast<uint32_t>(page_count); ++index) {
			if (response.hook_ids[index] == 0u) continue;
			sparse.push_back(Json{{"pageIndex", index},
								  {"hookId", hex_u64(response.hook_ids[index])}});
		}
		result["hooks"] = std::move(sparse);
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.unhook") {
		if (!exact_fields(arguments, {"hookId", "confirmed"}, error) ||
			!confirmed_true(arguments, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t hook_id = 0u;
		if (!parse_hex_u64(arguments, "hookId", hook_id, error) ||
			!hook_id_valid(hook_id)) {
			if (!error.contains("path"))
				error = Json{{"error", "hookId is not a valid generated VT hook identifier"},
							 {"path", "$.hookId"}};
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		if (!mutation_preflight_locked(tool_name, result)) return SAO_AI_EDITOR_OK;
		SaoRtIoVtProxyUnhookResponse response{};
		CallResult call{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_unhook(
			runtime, hook_id, kTimeoutMs, &response, &call);
		const bool typed = component_response_present(response.prefix);
		const FenceResult fence = fence_call_locked(status, call_epoch, typed);
		const bool fence_unknown = fence.failed || fence.transitioned;
		const uint32_t flags = response.prefix.response_flags |
			(fence_unknown ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN : 0u);
		const sao_status_t effective_status = fence_unknown
			? SAO_RT_IO_ERR_MUTATION_UNKNOWN
			: status;
		invalidate_cache_locked(status != SAO_STATUS_OK || fence_unknown ||
								(flags & (SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN |
											  SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_RECOVERY_REQUIRED)) != 0u);
		result = effective_status == SAO_STATUS_OK
					 ? success_result()
					 : operation_failure(effective_status, flags, call, true, typed);
		if (effective_status == SAO_STATUS_OK) {
			result["hookId"] = hex_u64(hook_id);
		} else {
			result["requestedHookId"] = hex_u64(hook_id);
		}
		if (typed) result["operationStatus"] = response.prefix.operation_status;
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.readPhys") {
		if (!exact_fields(arguments, {"gpa", "length"}, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t gpa = 0u;
		uint64_t length = 0u;
		if (!parse_hex_u64(arguments, "gpa", gpa, error) ||
			!parse_integer_range(arguments, "length", 1u,
								 SAO_RT_IO_VT_PROXY_MAX_TRANSFER, length, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t end = 0u;
		if (!checked_inclusive_end(gpa, length, end)) {
			result = Json{{"error", "gpa plus length overflows"}, {"path", "$.gpa"}};
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(length), 0u);
		SaoRtIoSecretScope bytes_wipe(bytes.data(), bytes.size());
		const sao_status_t runtime_status = sync_epoch_locked();
		if (runtime_status != SAO_STATUS_OK) {
			result = operation_failure(runtime_status, 0u, CallResult{}, false);
			return SAO_AI_EDITOR_OK;
		}
		SaoRtIoVtProxyTransferResponse response{};
		CallResult call{};
		size_t bytes_read = 0u;
		sao_status_t status = SAO_STATUS_OK;
		bool typed = false;
		for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
			response = SaoRtIoVtProxyTransferResponse{};
			call = CallResult{};
			bytes_read = 0u;
			const uint64_t call_epoch = epoch;
			status = sao_rt_io_proxy_runtime_vt_read_phys(
				runtime, gpa, bytes.size(), bytes.data(), bytes.size(), &bytes_read,
				kTimeoutMs, &response, &call);
			typed = component_response_present(response.prefix);
			const FenceResult fence = fence_call_locked(status, call_epoch, typed);
			if (fence.failed || (fence.transitioned && attempt != 0u)) {
				response = SaoRtIoVtProxyTransferResponse{};
				call = CallResult{};
				bytes_read = 0u;
				status = SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
				typed = false;
				break;
			}
			if (fence.transitioned) continue;
			break;
		}
		result = status == SAO_STATUS_OK
					 ? success_result()
					 : operation_failure(status, response.prefix.response_flags,
										 call, false, typed);
		result["bytesCompleted"] = response.bytes_completed;
		result["bytesRead"] = bytes_read;
		result["dataHex"] = hex_bytes(bytes.data(), bytes_read);
		if (typed) result["operationStatus"] = response.prefix.operation_status;
		return SAO_AI_EDITOR_OK;
	}
	if (tool_name == "vt.writePhys") {
		if (!exact_fields(arguments, {"gpa", "length", "dataHex", "confirmed"}, error) ||
			!confirmed_true(arguments, error)) {
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		uint64_t gpa = 0u;
		uint64_t length = 0u;
		std::vector<uint8_t> bytes;
		const bool parsed_gpa = parse_hex_u64(arguments, "gpa", gpa, error);
		const bool parsed_length = parsed_gpa &&
			parse_integer_range(arguments, "length", 1u,
								SAO_RT_IO_VT_PROXY_MAX_TRANSFER, length, error);
		uint64_t end = 0u;
		const bool valid_span = parsed_length && checked_inclusive_end(gpa, length, end);
		if (!valid_span) {
			if (!error.contains("path")) {
				error = Json{{"error", "gpa plus length overflows"}, {"path", "$.gpa"}};
			}
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		const bool parsed_data = parse_hex_bytes(
			arguments, "dataHex", SAO_RT_IO_VT_PROXY_MAX_TRANSFER, bytes, error);
		SaoRtIoSecretScope bytes_wipe(bytes.data(), bytes.size());
		if (!parsed_gpa || !parsed_length || !parsed_data ||
			bytes.size() != length) {
			if (!error.contains("path")) {
				error = Json{{"error", "dataHex must decode to exactly length bytes"},
							 {"path", "$.dataHex"}};
			}
			result = std::move(error);
			return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
		}
		if (!mutation_preflight_locked(tool_name, result)) return SAO_AI_EDITOR_OK;
		SaoRtIoVtProxyTransferResponse response{};
		CallResult call{};
		const uint64_t call_epoch = epoch;
		const sao_status_t status = sao_rt_io_proxy_runtime_vt_write_phys(
			runtime, gpa, bytes.data(), bytes.size(), kTimeoutMs, &response, &call);
		const bool typed = component_response_present(response.prefix);
		const FenceResult fence = fence_call_locked(status, call_epoch, typed);
		const bool fence_unknown = fence.failed || fence.transitioned;
		const bool partial_evidence = typed && response.bytes_completed > 0u;
		const uint32_t flags = response.prefix.response_flags |
			(fence_unknown ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN : 0u) |
			(fence_unknown && partial_evidence
				 ? SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_PARTIAL
				 : 0u);
		const sao_status_t effective_status = fence_unknown
			? SAO_RT_IO_ERR_MUTATION_UNKNOWN
			: status;
		invalidate_cache_locked(status != SAO_STATUS_OK || fence_unknown ||
								(flags & (SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_UNKNOWN |
											  SAO_RT_IO_VT_PROXY_RESPONSE_FLAG_RECOVERY_REQUIRED)) != 0u);
		result = effective_status == SAO_STATUS_OK
					 ? success_result()
					 : operation_failure(effective_status, flags, call, true, typed);
		result["bytesCompleted"] = response.bytes_completed;
		result["bytesWritten"] = response.bytes_completed;
		if (typed) result["operationStatus"] = response.prefix.operation_status;
		return SAO_AI_EDITOR_OK;
	}
	result = Json{{"error", "unknown VT tool"}, {"path", "$"}};
	return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

Bridge::Bridge() : impl_(std::make_unique<Impl>()) {}

Bridge::~Bridge() {
	if (impl_ == nullptr) return;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	RuntimeHandle runtime = impl_->runtime;
	impl_->runtime = nullptr;
	if (runtime != nullptr) {
		sao_rt_io_proxy_runtime_destroy(runtime);
	}
}

int32_t Bridge::execute(std::string_view tool_name, const Json& arguments,
						Json& result) {
	if (impl_ == nullptr) {
		result = Json{{"error", "VT bridge is not initialized"}};
		return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
	}
	std::lock_guard<std::mutex> lock(impl_->mutex);
	try {
		return impl_->execute_locked(tool_name, arguments, result);
	} catch (const std::exception&) {
		result = Json{{"ok", false},
					  {"available", false},
					  {"reason", "operation_failed"}};
		return SAO_AI_EDITOR_OK;
	} catch (...) {
		result = Json{{"ok", false},
					  {"available", false},
					  {"reason", "operation_failed"}};
		return SAO_AI_EDITOR_OK;
	}
}

}  // namespace sao::ai_editor::vt
