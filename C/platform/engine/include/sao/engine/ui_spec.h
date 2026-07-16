// SAO Auto — declarative UI spec builder / normalizer.
//
// 1:1 port of `sao_auto/python/act_platform/ui_spec.py`.  A UI spec is
// a JSON-like tree of container + leaf nodes.  Plugins build specs;
// both the native Direct2D renderer and the (deprecated) legacy WebView
// consume the same normalized shape.
//
// This header exposes the *validation / normalization* API only.  The
// spec is constructed from plugin-side script languages via the SDK
// (`sdk/include/sao/sdk/sao_sdk_ui.h`).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAO_UI_SPEC_VERSION 1

// Same bounds as Python's ui_spec.py — a plugin spec that exceeds these
// is truncated, never rejected.
enum : size_t {
    SAO_UI_SPEC_MAX_DEPTH       = 8,
    SAO_UI_SPEC_MAX_NODES       = 400,
    SAO_UI_SPEC_MAX_TABLE_ROWS  = 200,
    SAO_UI_SPEC_MAX_TABLE_COLS  = 16,
    SAO_UI_SPEC_MAX_TEXT_LEN    = 4000,
    SAO_UI_SPEC_MAX_TITLE_LEN   = 200,
    SAO_UI_SPEC_MAX_CANVAS_OPS  = 4000,
    SAO_UI_SPEC_MAX_CANVAS_DIM  = 4096,
    SAO_UI_SPEC_MAX_LAYER_POS   = 32768,
    SAO_UI_SPEC_MAX_LAYER_Z     = 10000,
};

// Node kinds — must match ui_spec.py exactly.  See there for semantics.
enum sao_ui_node_kind_e : int32_t {
    // containers
    SAO_UI_KIND_PANEL = 0,
    SAO_UI_KIND_SECTION,
    SAO_UI_KIND_CARD,
    SAO_UI_KIND_ROW,
    SAO_UI_KIND_GROUP,
    // leaves
    SAO_UI_KIND_TEXT,
    SAO_UI_KIND_KV,
    SAO_UI_KIND_BAR,
    SAO_UI_KIND_BADGE,
    SAO_UI_KIND_DIVIDER,
    SAO_UI_KIND_SPACER,
    SAO_UI_KIND_BUTTON,
    SAO_UI_KIND_INPUT,
    SAO_UI_KIND_SLIDER,
    SAO_UI_KIND_TABLE,
    SAO_UI_KIND_CANVAS,
    SAO_UI_KIND_RGBA_FRAME,
};

// Normalize (safety-clamp) a spec.  Input & output are both UTF-8 JSON.
// The output buffer is written with a canonical, minified spec that
// downstream renderers can trust without re-validating.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_ui_spec_normalize(
    const uint8_t* input_json_utf8,
    size_t input_len,
    uint8_t* out_json_utf8,
    size_t out_capacity,
    size_t* out_bytes_written);

// Query the version an ABI consumer expects — plugins should refuse
// specs that don't match major.
SAO_ENGINE_API uint32_t SAO_ENGINE_CALL sao_engine_ui_spec_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
