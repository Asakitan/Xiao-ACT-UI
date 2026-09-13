include_guard(GLOBAL)

set_property(GLOBAL PROPERTY _SAO_RUNTIME_TARGET_BASENAME_TABLE
    "shared|sao_core|97bd001616b9e8ca"
    "shared|sao_platform_engine|a75f352837858b12"
    "shared|sao_platform_net|57480787a1948dad"
    "shared|sao_runtime_installer|8e090dcf5d8440db"
    "shared|sao_platform_sdk|5813f2d52b330ada"
    "shared|sao_platform_ui|ed0fa2d377a29333"
    "shared|sao_security_anti_screencap|ed63eee40e7b1fb4"
    "shared|sao_shell_protocol|0df83e9853a6efc9"
    "shared|sao_shell_crypter|cca47240e9b21602"
    "shared|sao_shell_packer|db640ec39f0ae0ce"
    "shared|sao_shell_integrator|739bbf97d4bb1890"
    "shared|sao_license_protocol|e4768f4f950e7bc7"
    "shared|sao_license_client|7bddbf75a3963c48"
    "shared|sao_license_sdk|4d49c8a403c085f1"
    "shared|sao_server_freetier|302a755b74dbcd5a"
    "shared|sao_plugin_ai_editor|4f1988ce57d0a1c0"
    "payload|SaoAuto|f577a5067a96c920")
set_property(GLOBAL PROPERTY _SAO_RUNTIME_BUNDLE_LEAF "ff22701a59858ebf")
set_property(GLOBAL PROPERTY _SAO_RUNTIME_PRODUCT_ID "SaoAuto")
set_property(GLOBAL PROPERTY _SAO_RUNTIME_HELPER_TARGETS
    sao_core
    sao_license_protocol
    sao_license_client
    sao_license_sdk)

function(_sao_runtime_get_name_table out_table)
    get_property(_sao_table GLOBAL PROPERTY _SAO_RUNTIME_TARGET_BASENAME_TABLE)
    if (NOT _sao_table)
        message(FATAL_ERROR "SAO runtime target name table is unavailable")
    endif()
    set(${out_table} "${_sao_table}" PARENT_SCOPE)
endfunction()

function(_sao_runtime_parse_name_entry entry out_kind out_target out_basename)
    string(REPLACE "|" ";" _sao_fields "${entry}")
    list(LENGTH _sao_fields _sao_field_count)
    if (NOT _sao_field_count EQUAL 3)
        message(FATAL_ERROR "Malformed SAO runtime target name entry")
    endif()
    list(GET _sao_fields 0 _sao_kind)
    list(GET _sao_fields 1 _sao_target)
    list(GET _sao_fields 2 _sao_basename)
    set(${out_kind} "${_sao_kind}" PARENT_SCOPE)
    set(${out_target} "${_sao_target}" PARENT_SCOPE)
    set(${out_basename} "${_sao_basename}" PARENT_SCOPE)
endfunction()

function(_sao_runtime_validate_basename label value)
    string(LENGTH "${value}" _sao_length)
    if (NOT _sao_length EQUAL 16 OR NOT "${value}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "${label} must be exactly 16 lowercase hexadecimal characters")
    endif()
endfunction()

function(_sao_runtime_validate_name_table
         out_shared_targets out_payload_target out_payload_basename)
    _sao_runtime_get_name_table(_sao_table)
    set(_sao_targets)
    set(_sao_basenames)
    set(_sao_shared_targets)
    set(_sao_payload_target)
    set(_sao_payload_basename)

    foreach (_sao_entry IN LISTS _sao_table)
        _sao_runtime_parse_name_entry(
            "${_sao_entry}" _sao_kind _sao_target _sao_basename)
        if (NOT _sao_kind STREQUAL "shared" AND
            NOT _sao_kind STREQUAL "payload")
            message(FATAL_ERROR
                "Unknown SAO runtime target kind for ${_sao_target}: ${_sao_kind}")
        endif()
        if ("${_sao_target}" STREQUAL "")
            message(FATAL_ERROR "SAO runtime target name must not be empty")
        endif()
        _sao_runtime_validate_basename(
            "SAO runtime basename for ${_sao_target}" "${_sao_basename}")

        list(FIND _sao_targets "${_sao_target}" _sao_target_index)
        if (NOT _sao_target_index EQUAL -1)
            message(FATAL_ERROR
                "Duplicate SAO runtime target mapping: ${_sao_target}")
        endif()
        list(FIND _sao_basenames "${_sao_basename}" _sao_basename_index)
        if (NOT _sao_basename_index EQUAL -1)
            message(FATAL_ERROR
                "Duplicate SAO runtime output basename: ${_sao_basename}")
        endif()
        list(APPEND _sao_targets "${_sao_target}")
        list(APPEND _sao_basenames "${_sao_basename}")

        if (_sao_kind STREQUAL "shared")
            list(APPEND _sao_shared_targets "${_sao_target}")
        else()
            if (NOT "${_sao_payload_target}" STREQUAL "")
                message(FATAL_ERROR "SAO runtime name table has multiple payload targets")
            endif()
            set(_sao_payload_target "${_sao_target}")
            set(_sao_payload_basename "${_sao_basename}")
        endif()
    endforeach()

    if ("${_sao_payload_target}" STREQUAL "")
        message(FATAL_ERROR "SAO runtime name table has no payload target")
    endif()
    list(LENGTH _sao_shared_targets _sao_shared_target_count)
    if (NOT _sao_shared_target_count EQUAL 16)
        message(FATAL_ERROR
            "SAO runtime name table must contain exactly 16 shared targets")
    endif()
    if (NOT _sao_payload_target STREQUAL "SaoAuto")
        message(FATAL_ERROR
            "SAO runtime payload target must remain SaoAuto")
    endif()

    get_property(_sao_helper_targets
        GLOBAL PROPERTY _SAO_RUNTIME_HELPER_TARGETS)
    set(_sao_expected_helper_targets
        sao_core
        sao_license_protocol
        sao_license_client
        sao_license_sdk)
    if (NOT "${_sao_helper_targets}" STREQUAL
        "${_sao_expected_helper_targets}")
        message(FATAL_ERROR
            "SAO helper runtime target set does not match the frozen contract")
    endif()
    foreach (_sao_helper_target IN LISTS _sao_helper_targets)
        list(FIND _sao_shared_targets
            "${_sao_helper_target}" _sao_helper_shared_index)
        if (_sao_helper_shared_index EQUAL -1)
            message(FATAL_ERROR
                "SAO helper runtime target is not mapped as shared: "
                "${_sao_helper_target}")
        endif()
    endforeach()

    get_property(_sao_bundle_leaf GLOBAL PROPERTY _SAO_RUNTIME_BUNDLE_LEAF)
    _sao_runtime_validate_basename(
        "SAO runtime bundle leaf" "${_sao_bundle_leaf}")
    list(FIND _sao_basenames "${_sao_bundle_leaf}" _sao_bundle_collision)
    if (NOT _sao_bundle_collision EQUAL -1)
        message(FATAL_ERROR
            "SAO runtime bundle leaf collides with an output basename")
    endif()

    set(${out_shared_targets} "${_sao_shared_targets}" PARENT_SCOPE)
    set(${out_payload_target} "${_sao_payload_target}" PARENT_SCOPE)
    set(${out_payload_basename} "${_sao_payload_basename}" PARENT_SCOPE)
endfunction()

function(_sao_runtime_lookup_basename wanted_target out_basename)
    _sao_runtime_get_name_table(_sao_table)
    set(_sao_result)
    foreach (_sao_entry IN LISTS _sao_table)
        _sao_runtime_parse_name_entry(
            "${_sao_entry}" _sao_kind _sao_target _sao_basename)
        if (_sao_target STREQUAL "${wanted_target}")
            set(_sao_result "${_sao_basename}")
            break()
        endif()
    endforeach()
    if ("${_sao_result}" STREQUAL "")
        message(FATAL_ERROR
            "SAO runtime target has no output basename: ${wanted_target}")
    endif()
    set(${out_basename} "${_sao_result}" PARENT_SCOPE)
endfunction()

function(_sao_runtime_validate_hex_cache cache_name value)
    string(LENGTH "${value}" _sao_length)
    if (NOT _sao_length EQUAL 64 OR NOT "${value}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "${cache_name} must be exactly 64 lowercase hexadecimal characters")
    endif()
endfunction()

function(_sao_runtime_make_key_shares key_hex mask_hex out_share_one out_share_two)
    set(_sao_hex_digits "0123456789abcdef")
    set(_sao_share_one)
    set(_sao_share_two)
    foreach (_sao_index RANGE 0 31)
        math(EXPR _sao_offset "${_sao_index} * 2")
        string(SUBSTRING "${key_hex}" ${_sao_offset} 2 _sao_key_byte)
        string(SUBSTRING "${mask_hex}" ${_sao_offset} 2 _sao_mask_byte)
        math(EXPR _sao_masked_byte
            "0x${_sao_key_byte} ^ 0x${_sao_mask_byte}")
        math(EXPR _sao_high_nibble "${_sao_masked_byte} / 16")
        math(EXPR _sao_low_nibble "${_sao_masked_byte} % 16")
        string(SUBSTRING "${_sao_hex_digits}" ${_sao_high_nibble} 1 _sao_high_hex)
        string(SUBSTRING "${_sao_hex_digits}" ${_sao_low_nibble} 1 _sao_low_hex)
        list(APPEND _sao_share_one "0x${_sao_mask_byte}")
        list(APPEND _sao_share_two "0x${_sao_high_hex}${_sao_low_hex}")
    endforeach()
    string(JOIN ", " _sao_share_one_text ${_sao_share_one})
    string(JOIN ", " _sao_share_two_text ${_sao_share_two})
    set(${out_share_one} "${_sao_share_one_text}" PARENT_SCOPE)
    set(${out_share_two} "${_sao_share_two_text}" PARENT_SCOPE)
endfunction()

function(sao_runtime_initialize)
    _sao_runtime_validate_name_table(
        _sao_shared_targets _sao_payload_target _sao_payload_basename)

    if (DEFINED CACHE{SAO_RUNTIME_BUNDLE_KEY_HEX})
        get_property(_sao_key_hex
            CACHE SAO_RUNTIME_BUNDLE_KEY_HEX PROPERTY VALUE)
    elseif (DEFINED SAO_RUNTIME_BUNDLE_KEY_HEX)
        set(_sao_key_hex "${SAO_RUNTIME_BUNDLE_KEY_HEX}")
    else()
        string(RANDOM LENGTH 64 ALPHABET "0123456789abcdef" _sao_key_hex)
    endif()
    _sao_runtime_validate_hex_cache(
        "SAO_RUNTIME_BUNDLE_KEY_HEX" "${_sao_key_hex}")
    set(SAO_RUNTIME_BUNDLE_KEY_HEX "${_sao_key_hex}" CACHE INTERNAL
        "CNG key for the authenticated SAO runtime bundle" FORCE)

    if (DEFINED CACHE{SAO_RUNTIME_BUNDLE_MASK_HEX})
        get_property(_sao_mask_hex
            CACHE SAO_RUNTIME_BUNDLE_MASK_HEX PROPERTY VALUE)
    elseif (DEFINED SAO_RUNTIME_BUNDLE_MASK_HEX)
        set(_sao_mask_hex "${SAO_RUNTIME_BUNDLE_MASK_HEX}")
    else()
        string(RANDOM LENGTH 64 ALPHABET "0123456789abcdef" _sao_mask_hex)
    endif()
    _sao_runtime_validate_hex_cache(
        "SAO_RUNTIME_BUNDLE_MASK_HEX" "${_sao_mask_hex}")
    set(SAO_RUNTIME_BUNDLE_MASK_HEX "${_sao_mask_hex}" CACHE INTERNAL
        "Independent XOR mask for the SAO runtime bundle key" FORCE)

    _sao_runtime_make_key_shares(
        "${_sao_key_hex}" "${_sao_mask_hex}"
        SAO_RUNTIME_KEY_SHARE_ONE SAO_RUNTIME_KEY_SHARE_TWO)

    get_property(SAO_RUNTIME_BUNDLE_LEAF
        GLOBAL PROPERTY _SAO_RUNTIME_BUNDLE_LEAF)
    get_property(SAO_RUNTIME_PRODUCT_ID
        GLOBAL PROPERTY _SAO_RUNTIME_PRODUCT_ID)
    get_property(_sao_helper_targets
        GLOBAL PROPERTY _SAO_RUNTIME_HELPER_TARGETS)

    _sao_runtime_lookup_basename("sao_core" SAO_RUNTIME_CORE_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_platform_engine" SAO_RUNTIME_PLATFORM_ENGINE_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_platform_net" SAO_RUNTIME_PLATFORM_NET_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_runtime_installer" SAO_RUNTIME_INSTALLER_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_platform_sdk" SAO_RUNTIME_PLATFORM_SDK_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_platform_ui" SAO_RUNTIME_PLATFORM_UI_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_security_anti_screencap" SAO_RUNTIME_ANTI_SCREENCAP_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_shell_protocol" SAO_RUNTIME_SHELL_PROTOCOL_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_shell_crypter" SAO_RUNTIME_SHELL_CRYPTER_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_shell_packer" SAO_RUNTIME_SHELL_PACKER_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_shell_integrator" SAO_RUNTIME_SHELL_INTEGRATOR_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_license_protocol" SAO_RUNTIME_LICENSE_PROTOCOL_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_license_client" SAO_RUNTIME_LICENSE_CLIENT_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_license_sdk" SAO_RUNTIME_LICENSE_SDK_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_server_freetier" SAO_RUNTIME_SERVER_FREETIER_BASENAME)
    _sao_runtime_lookup_basename(
        "sao_plugin_ai_editor" SAO_RUNTIME_PLUGIN_AI_EDITOR_BASENAME)
    if (SAO_ENCRYPTED_RUNTIME_BUNDLE)
        set(SAO_RUNTIME_PAYLOAD_BASENAME "${_sao_payload_basename}")
    else()
        set(SAO_RUNTIME_PAYLOAD_BASENAME "SaoAuto")
    endif()

    set(SAO_RUNTIME_GENERATED_INCLUDE_DIR
        "${CMAKE_BINARY_DIR}/generated")
    set(_sao_generated_header
        "${SAO_RUNTIME_GENERATED_INCLUDE_DIR}/sao/runtime/runtime_key.h")
    file(MAKE_DIRECTORY
        "${SAO_RUNTIME_GENERATED_INCLUDE_DIR}/sao/runtime")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/runtime_names.h.in"
        "${_sao_generated_header}"
        @ONLY NEWLINE_STYLE LF)

    set(SAO_RUNTIME_GENERATED_INCLUDE_DIR
        "${SAO_RUNTIME_GENERATED_INCLUDE_DIR}" PARENT_SCOPE)
    set(SAO_RUNTIME_BUNDLE_LEAF "${SAO_RUNTIME_BUNDLE_LEAF}" PARENT_SCOPE)
    set(SAO_RUNTIME_PAYLOAD_TARGET "${_sao_payload_target}" PARENT_SCOPE)
    set(SAO_RUNTIME_SHARED_TARGETS "${_sao_shared_targets}" PARENT_SCOPE)
    set(SAO_RUNTIME_HELPER_TARGETS "${_sao_helper_targets}" PARENT_SCOPE)
endfunction()

function(sao_runtime_apply_output_names)
    _sao_runtime_validate_name_table(
        _sao_shared_targets _sao_payload_target _sao_payload_basename)
    _sao_runtime_get_name_table(_sao_table)

    foreach (_sao_entry IN LISTS _sao_table)
        _sao_runtime_parse_name_entry(
            "${_sao_entry}" _sao_kind _sao_target _sao_basename)
        if (NOT TARGET ${_sao_target})
            continue()
        endif()
        if (_sao_kind STREQUAL "payload" AND
            NOT SAO_ENCRYPTED_RUNTIME_BUNDLE)
            continue()
        endif()
        get_target_property(_sao_target_type ${_sao_target} TYPE)
        if (_sao_kind STREQUAL "shared" AND
            NOT _sao_target_type STREQUAL "SHARED_LIBRARY")
            message(FATAL_ERROR
                "Mapped SAO runtime target is not SHARED: ${_sao_target}")
        endif()
        if (_sao_kind STREQUAL "payload" AND
            NOT _sao_target_type STREQUAL "EXECUTABLE")
            message(FATAL_ERROR
                "Mapped SAO runtime payload is not executable: ${_sao_target}")
        endif()
        set_target_properties(${_sao_target} PROPERTIES
            OUTPUT_NAME "${_sao_basename}")
    endforeach()
endfunction()
