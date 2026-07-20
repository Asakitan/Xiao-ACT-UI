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

    // Append-only ABI 1.5 extension.  Fetch the current matrix lock's
    // heap coordinates so callers can persist them across sessions:
    //   *out_heap_base = 0, *out_heap_size = 0 when not locked.
    // *out_offset is only valid when *out_locked = 1.  Consumers store
    // {heap_size, offset} as a hint keyed on the target game and feed
    // it back via set_prior_lock_hint on the next attach for the same
    // game so cold-start lock latency drops from ~0.5-2 s full-scan to
    // "size-matching heap scanned first".
    sao_sdk_status_t(SAO_SDK_CALL* get_matrix_lock_info)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t* out_heap_base, uint32_t* out_offset,
        uint64_t* out_heap_size, uint8_t* out_locked);

    // Provide a hint from a previous session.  Takes effect on the
    // NEXT attach (candidate heaps whose size matches hint_heap_size
    // scan first).  Passing hint_heap_size = 0 clears any stored hint.
    // hint_offset is retained verbatim for future extension (per-
    // offset vote seeding); safe to pass 0 today.
    sao_sdk_status_t(SAO_SDK_CALL* set_prior_lock_hint)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t hint_heap_size, uint32_t hint_offset);

    // Append-only ABI 1.6 extension: cross-session skeleton identity.
    //
    // Snapshot the currently-locked primary skeleton's fingerprint
    // (sorted top-N adjacent bone distances, see optim 11).  Writes
    // up to max_count floats into out_floats and always sets
    // *out_count to the true fingerprint length.  Pass out_floats=NULL
    // to query the length without copying.  Returns SAO_SDK_OK with
    // *out_count=0 when there is no in-session fingerprint yet.
    sao_sdk_status_t(SAO_SDK_CALL* get_skeleton_fingerprint)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        float* out_floats, size_t max_count, size_t* out_count);

    // Feed a previously-saved skeleton fingerprint back into the
    // prior-lock hint.  Merges with any existing hint size / offset
    // set via set_prior_lock_hint.  Effect: on the next attach's
    // first rescan_bones the tracker will fuzzy-match candidate
    // clusters against this fingerprint and prefer the closest one
    // (see prior_fingerprint_match_max_diff in GpuTrackerConfig).
    // Passing count=0 (or floats=NULL) clears just the fingerprint
    // slice of the hint without touching heap_size / offset.
    sao_sdk_status_t(SAO_SDK_CALL* set_skeleton_fingerprint_hint)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        const float* floats, size_t count);

    // Append-only ABI 1.7 extension: multi-cluster access.
    //
    // Report how many bone clusters the tracker retained on the last
    // rescan (primary + top-K secondaries).  Zero when the tracker
    // has no lock or bone_clusters_snapshot is empty.
    sao_sdk_status_t(SAO_SDK_CALL* get_bone_cluster_count)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        size_t* out_count);

    // Copy up to max_bones (xyz triplets) of the cluster at `index`
    // into out_positions_xyz.  index 0 = primary (matches
    // get_skeleton_positions), 1..count-1 = secondaries in
    // descending-score order.  *out_bone_count reports the true
    // cluster size regardless of copy limit; pass out_positions_xyz
    // = NULL to query the length only.  Returns SAO_SDK_ERR_
    // INVALID_ARGUMENT if index is out of range.
    sao_sdk_status_t(SAO_SDK_CALL* get_bone_cluster_positions)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        size_t index, float* out_positions_xyz, size_t max_bones,
        size_t* out_bone_count);

    // Append-only ABI 1.8 extension: hot-heap fingerprint list.
    //
    // Each hot heap = 4 x uint64 flat: (size, protect, region_type,
    // header_8b).  protect and region_type are promoted to 64-bit for
    // flat ABI simplicity (Windows values are 32-bit, high bits stay
    // 0).  Callers persist this list to disk keyed on game and feed
    // it back on the next attach so the tracker jumps straight onto
    // known-good upload heaps by size + protect + region_type +
    // heap-header 8B fingerprint.
    //
    // get: writes up to max_entries * 4 u64 into out_flat and always
    // sets *out_entry_count to the true length.  Pass out_flat=NULL
    // to probe length.
    sao_sdk_status_t(SAO_SDK_CALL* get_prior_hot_heaps)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t* out_flat, size_t max_entries, size_t* out_entry_count);

    // set: replace the prior_hint.hot_heaps list with the caller's
    // array.  entry_count is entries (each 4 x u64).  Pass count=0
    // (or flat=NULL) to clear just the hot_heaps slice.  Existing
    // heap_size / offset / fingerprint parts of the hint are
    // preserved (read-modify-write).
    sao_sdk_status_t(SAO_SDK_CALL* set_prior_hot_heaps)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        const uint64_t* flat, size_t entry_count);

    // Append-only ABI 1.9 extension: interactive user pin + runtime
    // toggles.
    //
    // set_pinned_heap: force the tracker to prioritize `heap_base` on
    // every candidate scan.  heap_size is stored for diagnostics.
    // pinned_only != 0 collapses the per-tick sweep to just the pinned
    // heap.  Passing heap_base = 0 clears the pin (pinned_only ignored).
    sao_sdk_status_t(SAO_SDK_CALL* set_pinned_heap)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t heap_base, uint64_t heap_size, uint8_t pinned_only);

    // get_pinned_heap: report the current user pin.  *out_active = 1
    // when a pin is set, 0 otherwise (in which case the other outputs
    // are 0).  Any output pointer may be NULL to skip that field.
    sao_sdk_status_t(SAO_SDK_CALL* get_pinned_heap)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t* out_heap_base, uint64_t* out_heap_size,
        uint8_t* out_pinned_only, uint8_t* out_active);

    // Runtime toggle bits.  See SAO_SDK_GPU_HUNT_TOGGLE_* below.  The
    // caller reads the current mask with get_runtime_toggles, twiddles
    // some bits, and writes back with set_runtime_toggles(mask, changed
    // _mask).  Bits absent from changed_mask are left alone so a stale
    // read never overwrites a concurrent flip.
    sao_sdk_status_t(SAO_SDK_CALL* get_runtime_toggles)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t* out_mask);

    sao_sdk_status_t(SAO_SDK_CALL* set_runtime_toggles)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t mask, uint64_t changed_mask);

    // Append-only ABI 1.10 extension: HeapLocator engine profile.
    //
    // Mirrors sao::gpu_hunt::HeapLocatorProfile (D3D12Upload=0,
    // UnitySrp=1, DX11Dynamic=2, VulkanHost=3, Generic=4).  Setting
    // rebuilds the underlying HeapLocator and drops any live matrix
    // / bone locks so the next tick re-enumerates against the new
    // preset - callers can therefore flip Unity/DX11/Vulkan at
    // runtime from a UI selector without recreating the tracker.
    sao_sdk_status_t(SAO_SDK_CALL* get_locator_profile)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint32_t* out_profile);

    sao_sdk_status_t(SAO_SDK_CALL* set_locator_profile)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint32_t profile);

    // ABI 1.11 extension: view/proj 分离锁.
    //
    // Optim 28 replaces the single-lock get_view_proj() model with two
    // independent locks — view* alone and projection alone can be
    // acquired, persisted, and reported separately, so a plugin can
    // continue rendering ESP from a stable projection lock while the
    // view matrix re-locks after a camera cut.  camera_world_pos gives
    // callers a direct camera position in world space (no need to
    // decompose the view matrix themselves).  split_lock_info returns
    // per-matrix heap coordinates so the caller can persist and re-
    // apply each lock independently across sessions.

    // Fetch just the locked view matrix (16 floats, column-major).
    // `*out_locked` = 1 if the view lock is present, 0 otherwise; the
    // matrix contents are undefined when locked=0.
    sao_sdk_status_t(SAO_SDK_CALL* get_view_matrix)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        float out_matrix[16], uint8_t* out_locked);

    // Fetch just the locked projection matrix (16 floats, column-major).
    // `*out_locked` = 1 if the projection lock is present, 0 otherwise;
    // the matrix contents are undefined when locked=0.
    sao_sdk_status_t(SAO_SDK_CALL* get_proj_matrix)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        float out_matrix[16], uint8_t* out_locked);

    // Fetch the camera world position (3 floats).  Unlike the older
    // get_camera_pos which required a combined view*proj lock, this
    // slot is populated as soon as the view matrix alone is locked.
    // Sets `out_pos` to zeros and returns SAO_SDK_ERR_NOT_INITIALIZED
    // when the view lock is not present.
    sao_sdk_status_t(SAO_SDK_CALL* get_camera_world_pos)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        float out_pos[3]);

    // Snapshot the split view/proj lock coordinates for cross-session
    // persistence.  Each matrix has its own {heap_base, offset,
    // locked} triple; when neither is locked all outputs are 0.  Any
    // output pointer may be NULL to skip that field.
    sao_sdk_status_t(SAO_SDK_CALL* get_split_lock_info)(
        void* ctx_impl, sao_sdk_gpu_tracker_t tracker,
        uint64_t* out_view_heap_base, uint32_t* out_view_offset,
        uint64_t* out_proj_heap_base, uint32_t* out_proj_offset,
        uint8_t* out_view_locked, uint8_t* out_proj_locked);

    // ABI 1.11 metadata is appended after every pre-existing slot so all
    // historical GPU table offsets remain unchanged.  Consumers validate
    // these fields before reading any slot from the current table contract.
    uint32_t abi_version;
    uint32_t struct_size;
};

#define SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MAJOR SAO_SDK_ABI_VERSION_MAJOR
#define SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MINOR SAO_SDK_ABI_VERSION_MINOR
#define SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION                                                   \
    ((SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MAJOR << 16) |                                      \
     SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION_MINOR)

#define SAO_SDK_GPU_HUNT_TABLE_LEGACY_SIZE offsetof(struct SaoSdkGpuHuntTable, abi_version)
#define SAO_SDK_GPU_HUNT_TABLE_REQUIRED_SIZE                                                 \
    (offsetof(struct SaoSdkGpuHuntTable, struct_size) +                                      \
     sizeof(((struct SaoSdkGpuHuntTable*)0)->struct_size))

// HeapLocatorProfile constants (ABI 1.10).  Append-only; existing
// values never move.
#define SAO_SDK_GPU_HUNT_LOCATOR_PROFILE_D3D12_UPLOAD 0u
#define SAO_SDK_GPU_HUNT_LOCATOR_PROFILE_UNITY_SRP    1u
#define SAO_SDK_GPU_HUNT_LOCATOR_PROFILE_DX11_DYNAMIC 2u
#define SAO_SDK_GPU_HUNT_LOCATOR_PROFILE_VULKAN_HOST  3u
#define SAO_SDK_GPU_HUNT_LOCATOR_PROFILE_GENERIC      4u

// Runtime toggle bit definitions (ABI 1.9).  Append-only; existing
// bits never move.
#define SAO_SDK_GPU_HUNT_TOGGLE_BONE_REFRESH_PER_TICK        (1ULL << 0)
#define SAO_SDK_GPU_HUNT_TOGGLE_BONE_RESCAN_ONLY_LOCKED_HEAP (1ULL << 1)
#define SAO_SDK_GPU_HUNT_TOGGLE_BONE_PERSISTENCE_BIAS        (1ULL << 2)
#define SAO_SDK_GPU_HUNT_TOGGLE_RESTRICT_PRIMARY_TO_ACTOR    (1ULL << 3)
#define SAO_SDK_GPU_HUNT_TOGGLE_PROBE_HEAP_HEADER_8B         (1ULL << 4)
#define SAO_SDK_GPU_HUNT_TOGGLE_BONE_RESCAN_ON_HEAP_CHURN    (1ULL << 5)
#define SAO_SDK_GPU_HUNT_TOGGLE_INVALIDATE_ON_LOCKED_LOST    (1ULL << 6)
#define SAO_SDK_GPU_HUNT_TOGGLE_HEAP_CHURN_CHECK_ENABLED     (1ULL << 7)
#define SAO_SDK_GPU_HUNT_TOGGLE_HOT_HEAPS_LRU_ENABLED        (1ULL << 8)
// Optim 26: anti-cheat trap-page defense.  When set, tracker probes
// backend->probe_page_valid() on every 4 KiB page and skips pages
// with Valid=0 (WSL_PAGE_VALID clear) so external RPM never faults
// AC-planted trap pages into the target's working set.
#define SAO_SDK_GPU_HUNT_TOGGLE_SKIP_INVALID_WS_PAGES        (1ULL << 9)

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
