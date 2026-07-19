// SAO Auto — plugin context vtable.
//
// The single struct the platform hands to a plugin at load time.  Every
// capability the plugin can access is a function pointer in this
// vtable — the platform binds them at export.  This means plugins
// never link against sao_core / sao_engine / sao_ui directly; they
// only depend on this header.
//
// Growing a capability is a strictly append-only operation.  Never
// reorder or delete fields.  Bump the SDK minor version.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/sdk/sao_sdk_version.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward-declared handle types.  Real definitions live in the
// respective platform modules — plugins treat these as opaque.
typedef struct sao_sdk_ui_panel_s* sao_sdk_ui_panel_t;
typedef struct sao_sdk_ui_overlay_s* sao_sdk_ui_overlay_t;
typedef struct sao_sdk_ui_widget_s* sao_sdk_ui_widget_t;
typedef struct sao_sdk_config_scope_s* sao_sdk_config_scope_t;

struct SaoSdkMemoryTargetIdentity;
struct SaoSdkMemoryModule;
struct SaoSdkNetCaptureConfig;
struct SaoSdkNetPacketView;
struct SaoSdkNetParsedResult;

// GPU tracker handle — opaque 64-bit token issued by the SDK's per-context
// registry.  Never a raw pointer, so a plugin holding a stale handle after
// destroy_tracker gets SAO_SDK_ERR_INVALID_ARGUMENT rather than a UAF.
typedef uint64_t sao_sdk_gpu_tracker_t;

typedef int32_t sao_sdk_status_t;

// Status code semantics — thin subset of sao_status_t (see status.h).
enum : sao_sdk_status_t {
    SAO_SDK_OK = 0,
    SAO_SDK_ERR_INVALID_ARGUMENT = -1,
    SAO_SDK_ERR_NOT_INITIALIZED = -2,
    SAO_SDK_ERR_HANDLE_INVALID = -3,
    SAO_SDK_ERR_BUFFER_TOO_SMALL = -4,
    SAO_SDK_ERR_NOT_IMPLEMENTED = -5,
    SAO_SDK_ERR_ABI_MISMATCH = -9,
    SAO_SDK_ERR_UNSUPPORTED = -10,
    SAO_SDK_ERR_INTERNAL = -11,
    SAO_SDK_ERR_BUSY = -12,
    SAO_SDK_ERR_NOT_FOUND = -22,
    SAO_SDK_ERR_ALREADY_EXISTS = -23,
    SAO_SDK_ERR_READ_FAULT = -41,
};

typedef uint64_t sao_sdk_subscription_t;
typedef uint64_t sao_sdk_hook_token_t;
typedef uint64_t sao_sdk_hotkey_id_t;

// Widget kind — subset of `sao_ui_widget_kind_e` in the ui module,
// exposed here so plugins never have to include ui/ headers.  Only the
// kinds needed by SDK-level demos are listed; new kinds are appended
// (never inserted) so the ABI stays stable.
enum sao_sdk_ui_widget_kind_e : int32_t {
    SAO_SDK_UI_WIDGET_LABEL = 0,
    SAO_SDK_UI_WIDGET_BUTTON = 1,
    SAO_SDK_UI_WIDGET_PROGRESS_BAR = 2,
    SAO_SDK_UI_WIDGET_TABLE = 3,
    SAO_SDK_UI_WIDGET_ROUNDED_PANEL = 4,
    SAO_SDK_UI_WIDGET_STATUS_BADGE = 5,
    SAO_SDK_UI_WIDGET_TEXT_FIELD = 6,
    SAO_SDK_UI_WIDGET_CHECKBOX = 7,
    SAO_SDK_UI_WIDGET_DIVIDER = 8,
    SAO_SDK_UI_WIDGET_ICON = 9,
};

// Descriptor for `sao_sdk_register_ui_panel` — mirrors the runtime
// SaoPanelDescriptor but only the fields the SDK exposes.  The rest of
// the descriptor is defaulted by the platform.
struct SaoSdkPanelDescriptor {
    const char* panel_id_utf8; // unique across process
    const char* title_utf8;
    int32_t default_x_px;
    int32_t default_y_px;
    int32_t default_width_px;
    int32_t default_height_px;
    int32_t min_width_px;
    int32_t min_height_px;
    bool movable;
    bool resizable;
    bool show_titlebar;
    bool show_close_button;
    bool visible; // initial visibility
    bool remember_geometry;
    bool modal;
    bool overlay_style; // click-through outside widget region
    int32_t z_class;    // 0 normal / 1 topmost / 2 bottom
    int32_t z_within_class;
    float initial_opacity; // 0..1; 0 → default 1.0
    uint8_t _pad[4];
};

// Descriptor for `sao_sdk_panel_add_widget` — one shape covers every
// widget kind.  Fields not consumed by the target kind are ignored.
struct SaoSdkWidgetSpec {
    int32_t kind;               // sao_sdk_ui_widget_kind_e
    const char* widget_id_utf8; // caller-defined identity; unique per panel
    // Optional label / text for label/button/badge/checkbox.
    const char* text_utf8;
    // Optional JSON props blob for kind-specific attributes (color,
    // font size, initial state, etc).  UTF-8, may be NULL.
    const uint8_t* props_json_utf8;
    size_t props_len;
    // Geometry hint — panel body layout may honour or ignore.
    int32_t x_px;
    int32_t y_px;
    int32_t width_px;
    int32_t height_px;
    // Progress bar family.
    float value;
    float max_value;
    // Ordering — higher z draws later within the panel body.
    int32_t z_order;
    uint8_t _pad[4];
};

// Event callback (JSON UTF-8).
typedef void(SAO_SDK_CALL* sao_sdk_event_callback_t)(const char* topic_utf8,
                                                     const uint8_t* json_payload_utf8,
                                                     size_t payload_len, void* user_data);

// Render-clock hook point — subset of `sao_ui_render_hook_point_e`
// exposed to plugins.  New points are appended (never inserted) so the
// SDK ABI stays byte-stable.
enum sao_sdk_render_hook_point_e : int32_t {
    SAO_SDK_HOOK_BEFORE_COMPOSITOR = 0,
    SAO_SDK_HOOK_AFTER_COMPOSITOR = 1,
    SAO_SDK_HOOK_BEFORE_PRESENT = 2,
    SAO_SDK_HOOK_AFTER_PRESENT = 3,
};

// Render-hook payload — trimmed subset of `SaoUiRenderHookPayload`.
struct SaoSdkRenderHookPayload {
    int64_t frame_time_us;
    uint32_t frame_index;
    uint32_t frame_delta_us;
    int32_t viewport_x_px;
    int32_t viewport_y_px;
    int32_t viewport_width_px;
    int32_t viewport_height_px;
    uint32_t dispatch_flags;
    uint32_t reserved;
};

struct SaoSdkRenderHookSpec {
    const char* surface_id_utf8;
    int32_t hook_point;
    float priority;
};

// Render-hook callback.  Returns 0 on success; non-zero to signal the
// host that the hook faulted (host logs + skips).
typedef sao_sdk_status_t(SAO_SDK_CALL* sao_sdk_render_hook_callback_t)(
    int32_t hook_point, // sao_sdk_render_hook_point_e
    const struct SaoSdkRenderHookPayload* payload, void* user_data);

typedef void(SAO_SDK_CALL* sao_sdk_hotkey_callback_t)(sao_sdk_hotkey_id_t hotkey_id,
                                                      void* user_data);

typedef void(SAO_SDK_CALL* sao_sdk_panel_action_callback_t)(const char* action_key_utf8,
                                                            const uint8_t* action_arg_json_utf8,
                                                            size_t action_arg_len, void* user_data);

typedef void(SAO_SDK_CALL* sao_sdk_net_packet_callback_t)(const struct SaoSdkNetPacketView* packet,
                                                          void* user_data);

// UI capability — register a plugin panel, overlays, request redraws.
struct SaoSdkUiTable {
    // Registers a new plugin panel.  spec_json_utf8 is the initial UI spec.
    sao_sdk_status_t(SAO_SDK_CALL* register_panel)(void* ctx_impl, const char* panel_id_utf8,
                                                   const char* title_utf8,
                                                   const uint8_t* initial_spec_json_utf8,
                                                   size_t spec_len,
                                                   sao_sdk_panel_action_callback_t action_cb,
                                                   void* action_user_data,
                                                   sao_sdk_ui_panel_t* out_panel);

    // Push a new spec to an existing panel.
    sao_sdk_status_t(SAO_SDK_CALL* set_panel_spec)(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                   const uint8_t* spec_json_utf8, size_t spec_len);

    // Register an overlay on a specific surface (panel id or "*").
    sao_sdk_status_t(SAO_SDK_CALL* set_overlay)(void* ctx_impl, const char* surface_id_utf8,
                                                const uint8_t* spec_json_utf8, size_t spec_len);

    // Register a render hook (legacy — surface-name based JSON hooks).
    sao_sdk_status_t(SAO_SDK_CALL* register_render_hook)(void* ctx_impl,
                                                         const char* surface_id_utf8,
                                                         float priority, void* hook_fn,
                                                         void* hook_user_data,
                                                         sao_sdk_hook_token_t* out_token);

    sao_sdk_status_t(SAO_SDK_CALL* unregister_render_hook)(void* ctx_impl,
                                                           sao_sdk_hook_token_t token);

    // Register a typed render-clock hook (Wave 7 — fires at
    // BEFORE_COMPOSITOR / AFTER_COMPOSITOR / BEFORE_PRESENT /
    // AFTER_PRESENT).
    sao_sdk_status_t(SAO_SDK_CALL* register_render_hook_clock)(
        void* ctx_impl,
        int32_t hook_point, // sao_sdk_render_hook_point_e
        sao_sdk_render_hook_callback_t callback, void* user_data, sao_sdk_hook_token_t* out_token);

    sao_sdk_status_t(SAO_SDK_CALL* request_redraw)(void* ctx_impl, const char* surface_id_utf8);

    // ─── Wave 7 append-only extension ────────────────────────────────
    // Descriptor-based panel registration + widget CRUD.  Legacy
    // register_panel() above stays wired for JSON-spec consumers; the
    // fields below are what typed-C plugins call through.

    // Register a panel using the platform's SaoPanelDescriptor path
    // (see ui/panel_sdk.h).  Handled by the SDK wire; forwards to
    // sao_ui_panel_register().
    sao_sdk_status_t(SAO_SDK_CALL* register_ui_panel)(
        void* ctx_impl, const struct SaoSdkPanelDescriptor* descriptor,
        sao_sdk_ui_panel_t* out_panel);

    sao_sdk_status_t(SAO_SDK_CALL* unregister_ui_panel)(void* ctx_impl, sao_sdk_ui_panel_t panel);

    sao_sdk_status_t(SAO_SDK_CALL* panel_add_widget)(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                     const struct SaoSdkWidgetSpec* widget_spec,
                                                     sao_sdk_ui_widget_t* out_widget);

    sao_sdk_status_t(SAO_SDK_CALL* panel_update_widget)(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                        sao_sdk_ui_widget_t widget,
                                                        const struct SaoSdkWidgetSpec* widget_spec);

    sao_sdk_status_t(SAO_SDK_CALL* panel_remove_widget)(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                        sao_sdk_ui_widget_t widget);
};

// Event capability — subscribe/publish onto the platform event bus.
struct SaoSdkEventTable {
    sao_sdk_status_t(SAO_SDK_CALL* subscribe)(void* ctx_impl, const char* topic_utf8,
                                              sao_sdk_event_callback_t callback, void* user_data,
                                              sao_sdk_subscription_t* out_subscription);

    sao_sdk_status_t(SAO_SDK_CALL* unsubscribe)(void* ctx_impl,
                                                sao_sdk_subscription_t subscription);

    sao_sdk_status_t(SAO_SDK_CALL* publish)(void* ctx_impl, const char* topic_utf8,
                                            const uint8_t* json_payload_utf8, size_t payload_len);
};

// Memory capability — read-only remote process access via the platform's
// central process handle.  Plugins never open their own process handles.
struct SaoSdkMemTable {
    sao_sdk_status_t(SAO_SDK_CALL* read)(void* ctx_impl, uint64_t address, void* out_buffer,
                                         size_t buffer_len, size_t* out_bytes_read);

    sao_sdk_status_t(SAO_SDK_CALL* read_u32)(void* ctx_impl, uint64_t address, uint32_t* out_value);

    sao_sdk_status_t(SAO_SDK_CALL* read_u64)(void* ctx_impl, uint64_t address, uint64_t* out_value);

    sao_sdk_status_t(SAO_SDK_CALL* read_ptr_chain)(void* ctx_impl, uint64_t base_address,
                                                   const int32_t* offsets, size_t offset_count,
                                                   uint64_t* out_final_address);

    // Query the module base by exact case-insensitive name (e.g.
    // "star_resonance.exe", "il2cpp_x64.dll").
    sao_sdk_status_t(SAO_SDK_CALL* module_base)(void* ctx_impl, const char* module_name_utf8,
                                                uint64_t* out_base);

    // ABI 1.4 fixed prefix ends immediately before these metadata fields.
    // ABI 1.5 consumers must validate both fields before reading any slot
    // below them.
    uint32_t abi_version;
    uint32_t struct_size;

    // Append-only ABI 1.5 extension. The context owns an independent memory
    // provider session; the plugin only selects the target identity and uses
    // read-only operations through that session.
    sao_sdk_status_t(SAO_SDK_CALL* attach)(void* ctx_impl,
                                           const struct SaoSdkMemoryTargetIdentity* identity);

    sao_sdk_status_t(SAO_SDK_CALL* detach)(void* ctx_impl);

    sao_sdk_status_t(SAO_SDK_CALL* enumerate_modules)(void* ctx_impl,
                                                      struct SaoSdkMemoryModule* out_modules,
                                                      size_t capacity, size_t element_stride,
                                                      size_t* out_count);
};

#define SAO_SDK_MEM_TABLE_ABI_VERSION_MAJOR 1u
#define SAO_SDK_MEM_TABLE_ABI_VERSION_MINOR 5u
#define SAO_SDK_MEM_TABLE_ABI_VERSION                                                              \
    ((SAO_SDK_MEM_TABLE_ABI_VERSION_MAJOR << 16) | SAO_SDK_MEM_TABLE_ABI_VERSION_MINOR)

#define SAO_SDK_MEM_TABLE_V1_4_SIZE offsetof(struct SaoSdkMemTable, abi_version)
#define SAO_SDK_MEM_TABLE_V1_5_REQUIRED_SIZE                                                       \
    (offsetof(struct SaoSdkMemTable, enumerate_modules) +                                          \
     sizeof(((struct SaoSdkMemTable*)0)->enumerate_modules))

// Net capability — subscribe to reassembled application frames from the
// packet pipeline.  Plugins never open their own capture.
struct SaoSdkNetTable {
    typedef void(SAO_SDK_CALL* frame_callback_t)(const uint8_t* frame_bytes, size_t frame_length,
                                                 uint64_t ts_unix_ns, void* user_data);

    sao_sdk_status_t(SAO_SDK_CALL* set_frame_callback)(void* ctx_impl, frame_callback_t callback,
                                                       void* user_data);

    // ABI 1.4 fixed prefix ends immediately before these metadata fields.
    // ABI 1.5 consumers must validate both fields before reading appended slots.
    uint32_t abi_version;
    uint32_t struct_size;

    sao_sdk_status_t(SAO_SDK_CALL* capture_start)(void* ctx_impl,
                                                  const struct SaoSdkNetCaptureConfig* config,
                                                  sao_sdk_net_packet_callback_t callback,
                                                  void* user_data);
    sao_sdk_status_t(SAO_SDK_CALL* capture_stop)(void* ctx_impl);
    sao_sdk_status_t(SAO_SDK_CALL* parse_packet)(void* ctx_impl,
                                                 const struct SaoSdkNetPacketView* packet,
                                                 struct SaoSdkNetParsedResult* out_results,
                                                 size_t capacity, size_t element_stride,
                                                 size_t* out_count);
};

#define SAO_SDK_NET_TABLE_ABI_VERSION_MAJOR 1u
#define SAO_SDK_NET_TABLE_ABI_VERSION_MINOR 5u
#define SAO_SDK_NET_TABLE_ABI_VERSION                                                              \
    ((SAO_SDK_NET_TABLE_ABI_VERSION_MAJOR << 16) | SAO_SDK_NET_TABLE_ABI_VERSION_MINOR)

#define SAO_SDK_NET_TABLE_V1_4_SIZE offsetof(struct SaoSdkNetTable, abi_version)
#define SAO_SDK_NET_TABLE_V1_5_REQUIRED_SIZE                                                       \
    (offsetof(struct SaoSdkNetTable, parse_packet) +                                               \
     sizeof(((struct SaoSdkNetTable*)0)->parse_packet))

// Config capability — scoped to the plugin's id.
struct SaoSdkConfigTable {
    sao_sdk_status_t(SAO_SDK_CALL* get_bool)(void* ctx_impl, const char* key_utf8, bool* out_value);

    sao_sdk_status_t(SAO_SDK_CALL* get_int)(void* ctx_impl, const char* key_utf8,
                                            int64_t* out_value);

    sao_sdk_status_t(SAO_SDK_CALL* get_double)(void* ctx_impl, const char* key_utf8,
                                               double* out_value);

    sao_sdk_status_t(SAO_SDK_CALL* get_string)(void* ctx_impl, const char* key_utf8,
                                               char* out_buffer, size_t buffer_len,
                                               size_t* out_bytes_needed);

    sao_sdk_status_t(SAO_SDK_CALL* set_bool)(void* ctx_impl, const char* key_utf8, bool value);

    sao_sdk_status_t(SAO_SDK_CALL* set_int)(void* ctx_impl, const char* key_utf8, int64_t value);

    sao_sdk_status_t(SAO_SDK_CALL* set_double)(void* ctx_impl, const char* key_utf8, double value);

    sao_sdk_status_t(SAO_SDK_CALL* set_string)(void* ctx_impl, const char* key_utf8,
                                               const char* value_utf8);
};

// Hotkey capability.
struct SaoSdkHotkeyTable {
    sao_sdk_status_t(SAO_SDK_CALL* register_hotkey)(void* ctx_impl, const char* hotkey_name_utf8,
                                                    uint32_t virtual_key, uint32_t modifier_mask,
                                                    sao_sdk_hotkey_callback_t callback,
                                                    void* user_data, sao_sdk_hotkey_id_t* out_id);

    sao_sdk_status_t(SAO_SDK_CALL* unregister_hotkey)(void* ctx_impl, sao_sdk_hotkey_id_t id);
};

// TTS capability.
struct SaoSdkTtsTable {
    sao_sdk_status_t(SAO_SDK_CALL* speak)(void* ctx_impl, const char* text_utf8, float volume,
                                          float rate);

    sao_sdk_status_t(SAO_SDK_CALL* stop)(void* ctx_impl);
};

// Banner capability — the fullscreen alert popup for boss mechanics.
struct SaoSdkBannerTable {
    sao_sdk_status_t(SAO_SDK_CALL* show)(void* ctx_impl, const char* text_utf8,
                                         uint32_t duration_ms, uint32_t argb_color);
};

// GPU hunt capability — DX12 upload-heap view/projection matrix and
// skeleton bone locator.  Plugins never speak rt_io directly; the
// tracker owns a provider session obtained from the context's retained
// SaoSdkProviderVTable.  Until the host supplies every GPU I/O slot,
// `create_tracker` fails closed with SAO_SDK_ERR_UNSUPPORTED.
//
// Vec3 / Mat4 are exposed as flat arrays through the SDK ABI so plugins
// in any language just deal with float[3] / float[16].  Mat4 is column-
// major (matches HLSL default; gpu_hunt uses the same convention).
struct SaoSdkGpuHuntTable {
    // Create a detached tracker.  Returns 0 handle on failure.  Call
    // attach_tracker before tick; tick on a detached tracker returns
    // SAO_SDK_ERR_NOT_INITIALIZED rather than reporting false progress.
    sao_sdk_status_t(SAO_SDK_CALL* create_tracker)(void* ctx_impl,
                                                   sao_sdk_gpu_tracker_t* out_tracker);

    sao_sdk_status_t(SAO_SDK_CALL* destroy_tracker)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker);

    // Drive one iteration.  Non-blocking.  Steady-state cost after
    // lock is a single 64-byte read via rt_io.
    sao_sdk_status_t(SAO_SDK_CALL* tick)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker);

    // Fetch the locked view*projection matrix (16 floats, column-major).
    // `*out_locked` is set to 1 if the tracker has a lock, 0 otherwise;
    // if locked=0 the matrix contents are undefined.
    sao_sdk_status_t(SAO_SDK_CALL* get_view_proj)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
                                                  float out_matrix[16], uint8_t* out_locked);

    // Fetch the approximate camera world position (3 floats).  Only
    // meaningful when the tracker has a matrix lock.
    sao_sdk_status_t(SAO_SDK_CALL* get_camera_pos)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
                                                   float out_pos[3]);

    // Copy up to `max_bones` bone positions (xyz triplets) into
    // `out_positions_xyz`.  `*out_bone_count` reports how many were
    // written.  If `out_positions_xyz` is NULL, only the count is set.
    sao_sdk_status_t(SAO_SDK_CALL* get_skeleton_positions)(void* ctx_impl,
                                                           sao_sdk_gpu_tracker_t tracker,
                                                           float* out_positions_xyz,
                                                           size_t max_bones,
                                                           size_t* out_bone_count);

    // Project a single world position to screen using the tracker's
    // locked matrix.  `*out_visible = 1` if the point is on-screen,
    // 0 otherwise (behind camera or outside frustum with 5% margin).
    sao_sdk_status_t(SAO_SDK_CALL* world_to_screen)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
                                                    const float world_pos[3], int32_t viewport_w,
                                                    int32_t viewport_h, float* out_screen_x,
                                                    float* out_screen_y, uint8_t* out_visible);

    // Explicit invalidation — call when the plugin knows the target
    // switched maps or restarted.  Safe to call at any time.
    sao_sdk_status_t(SAO_SDK_CALL* invalidate)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker);

    // Append-only ABI 1.4 extension.  The host provider performs the
    // process attach inside the retained per-tracker I/O session.
    sao_sdk_status_t(SAO_SDK_CALL* attach_tracker)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
                                                   uint32_t pid);

    sao_sdk_status_t(SAO_SDK_CALL* detach_tracker)(void* ctx_impl, sao_sdk_gpu_tracker_t tracker);
};

// The main context struct.  ctx_impl is an opaque pointer to the
// platform's per-plugin state; every function pointer takes it as
// the first argument.
struct SaoSdkContext {
    uint32_t abi_version; // == SAO_SDK_ABI_VERSION at load time
    uint32_t reserved0;
    void* ctx_impl; // opaque platform-side state

    const char* plugin_id_utf8;      // e.g. "star_resonance"
    const char* plugin_version_utf8; // e.g. "4.6.94"

    const struct SaoSdkUiTable* ui;
    const struct SaoSdkEventTable* event;
    const struct SaoSdkMemTable* mem;
    const struct SaoSdkNetTable* net;
    const struct SaoSdkConfigTable* config;
    const struct SaoSdkHotkeyTable* hotkey;
    const struct SaoSdkTtsTable* tts;
    const struct SaoSdkBannerTable* banner;
    const struct SaoSdkGpuHuntTable* gpu_hunt;
    // TODO(phase-6): scripting table for plugins that host scripts of
    // their own (e.g. the workshop plugin's LuaJit sandbox).
};

#ifdef __cplusplus
} // extern "C"
#endif
