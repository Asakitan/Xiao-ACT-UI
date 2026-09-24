if (NOT DEFINED SAO_RUNTIME_AUDIT_PACKER OR
    NOT DEFINED SAO_RUNTIME_AUDIT_BUNDLE OR
    NOT DEFINED SAO_RUNTIME_AUDIT_SHIP_DIRECTORY OR
    NOT DEFINED SAO_RUNTIME_AUDIT_BUILD_BOOTSTRAP OR
    NOT DEFINED SAO_RUNTIME_AUDIT_BOOTSTRAP OR
    NOT DEFINED SAO_RUNTIME_AUDIT_PES OR
    NOT DEFINED SAO_RUNTIME_AUDIT_FORBIDDEN_SHIP_FILES OR
    NOT DEFINED SAO_RUNTIME_AUDIT_EXPECTED_BUNDLE_LEAF OR
    NOT DEFINED SAO_RUNTIME_AUDIT_EXPECTED_PAYLOAD_LEAF)
    message(FATAL_ERROR "runtime bundle audit inputs are incomplete")
endif()

if (NOT EXISTS "${SAO_RUNTIME_AUDIT_PACKER}")
    message(FATAL_ERROR "runtime bundle verifier is missing: ${SAO_RUNTIME_AUDIT_PACKER}")
endif()
if (NOT EXISTS "${SAO_RUNTIME_AUDIT_BUNDLE}")
    message(FATAL_ERROR "runtime bundle is missing: ${SAO_RUNTIME_AUDIT_BUNDLE}")
endif()
if (NOT EXISTS "${SAO_RUNTIME_AUDIT_BUILD_BOOTSTRAP}" OR
    IS_DIRECTORY "${SAO_RUNTIME_AUDIT_BUILD_BOOTSTRAP}")
    message(FATAL_ERROR
        "build runtime bootstrap is missing: ${SAO_RUNTIME_AUDIT_BUILD_BOOTSTRAP}")
endif()
if (NOT EXISTS "${SAO_RUNTIME_AUDIT_BOOTSTRAP}" OR
    IS_DIRECTORY "${SAO_RUNTIME_AUDIT_BOOTSTRAP}")
    message(FATAL_ERROR
        "runtime bootstrap is missing: ${SAO_RUNTIME_AUDIT_BOOTSTRAP}")
endif()
get_filename_component(_sao_bootstrap_name
    "${SAO_RUNTIME_AUDIT_BOOTSTRAP}" NAME)
if (NOT _sao_bootstrap_name STREQUAL "SaoAuto.exe")
    message(FATAL_ERROR
        "runtime bootstrap filename must be exactly SaoAuto.exe")
endif()
get_filename_component(_sao_bundle_leaf "${SAO_RUNTIME_AUDIT_BUNDLE}" NAME)
string(LENGTH "${_sao_bundle_leaf}" _sao_bundle_leaf_length)
if (NOT "${_sao_bundle_leaf}" STREQUAL
    "${SAO_RUNTIME_AUDIT_EXPECTED_BUNDLE_LEAF}" OR
    NOT _sao_bundle_leaf_length EQUAL 16 OR
    NOT "${_sao_bundle_leaf}" MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "runtime bundle leaf does not match the frozen opaque name")
endif()
set(_sao_shipped_bundle
    "${SAO_RUNTIME_AUDIT_SHIP_DIRECTORY}/bin/runtime/${_sao_bundle_leaf}")
if (NOT EXISTS "${_sao_shipped_bundle}" OR
    IS_DIRECTORY "${_sao_shipped_bundle}")
    message(FATAL_ERROR
        "shipped runtime bundle is missing: ${_sao_shipped_bundle}")
endif()
file(SHA256 "${SAO_RUNTIME_AUDIT_BUNDLE}" _sao_build_bundle_sha256)
file(SHA256 "${_sao_shipped_bundle}" _sao_ship_bundle_sha256)
if (NOT "${_sao_build_bundle_sha256}" STREQUAL
    "${_sao_ship_bundle_sha256}")
    message(FATAL_ERROR
        "build and shipped runtime bundles differ")
endif()
file(SHA256 "${SAO_RUNTIME_AUDIT_BUILD_BOOTSTRAP}"
    _sao_build_bootstrap_sha256)
file(SHA256 "${SAO_RUNTIME_AUDIT_BOOTSTRAP}"
    _sao_ship_bootstrap_sha256)
if (NOT "${_sao_build_bootstrap_sha256}" STREQUAL
    "${_sao_ship_bootstrap_sha256}")
    message(FATAL_ERROR
        "build and shipped runtime bootstraps differ")
endif()

list(LENGTH SAO_RUNTIME_AUDIT_PES _sao_runtime_input_count)
list(LENGTH SAO_RUNTIME_AUDIT_FORBIDDEN_SHIP_FILES
    _sao_runtime_destination_count)
if (NOT _sao_runtime_input_count EQUAL 17 OR
    NOT _sao_runtime_destination_count EQUAL 21)
    message(FATAL_ERROR
        "runtime bundle audit requires 17 unique PE inputs and 21 destination records")
endif()

file(TIMESTAMP "${SAO_RUNTIME_AUDIT_BUNDLE}"
    _sao_bundle_timestamp "%Y%m%d%H%M%S%f" UTC)
if ("${_sao_bundle_timestamp}" STREQUAL "")
    message(FATAL_ERROR "runtime bundle timestamp is unavailable")
endif()
foreach (_sao_pe IN LISTS SAO_RUNTIME_AUDIT_PES)
    if (NOT EXISTS "${_sao_pe}" OR IS_DIRECTORY "${_sao_pe}")
        message(FATAL_ERROR "runtime PE is missing before encryption: ${_sao_pe}")
    endif()
    file(TIMESTAMP "${_sao_pe}" _sao_pe_timestamp "%Y%m%d%H%M%S%f" UTC)
    if ("${_sao_pe_timestamp}" STREQUAL "" OR
        "${_sao_pe_timestamp}" STRGREATER "${_sao_bundle_timestamp}")
        message(FATAL_ERROR
            "runtime bundle is older than a plaintext input: ${_sao_pe}")
    endif()
endforeach()

foreach (_sao_bundle_to_verify IN ITEMS
        "${SAO_RUNTIME_AUDIT_BUNDLE}" "${_sao_shipped_bundle}")
    execute_process(
        COMMAND "${SAO_RUNTIME_AUDIT_PACKER}" --verify "${_sao_bundle_to_verify}"
        RESULT_VARIABLE _sao_verify_result
        OUTPUT_VARIABLE _sao_verify_stdout
        ERROR_VARIABLE _sao_verify_stderr)
    if (NOT _sao_verify_result EQUAL 0)
        message(FATAL_ERROR
            "runtime bundle authentication failed (${_sao_verify_result}):\n"
            "${_sao_verify_stdout}${_sao_verify_stderr}")
    endif()
endforeach()

execute_process(
    COMMAND "${SAO_RUNTIME_AUDIT_PACKER}" --audit-key-layout
        "${SAO_RUNTIME_AUDIT_BOOTSTRAP}"
    RESULT_VARIABLE _sao_key_layout_result
    OUTPUT_VARIABLE _sao_key_layout_stdout
    ERROR_VARIABLE _sao_key_layout_stderr)
if (NOT _sao_key_layout_result EQUAL 0)
    message(FATAL_ERROR
        "runtime bootstrap key-layout audit failed (${_sao_key_layout_result}):\n"
        "${_sao_key_layout_stdout}${_sao_key_layout_stderr}")
endif()

file(SIZE "${_sao_shipped_bundle}" _sao_bundle_size)
if (_sao_bundle_size LESS 128)
    message(FATAL_ERROR "runtime bundle is truncated")
endif()

file(GLOB_RECURSE _sao_ship_plaintext_dlls LIST_DIRECTORIES FALSE
    "${SAO_RUNTIME_AUDIT_SHIP_DIRECTORY}/bin/sao_*.dll"
    "${SAO_RUNTIME_AUDIT_SHIP_DIRECTORY}/bin/runtime/sao_*.dll")
if (_sao_ship_plaintext_dlls)
    message(FATAL_ERROR
        "descriptive first-party DLL leaked into ship tree: ${_sao_ship_plaintext_dlls}")
endif()

if (DEFINED SAO_RUNTIME_AUDIT_FORBIDDEN_SHIP_FILES)
    foreach (_sao_forbidden IN LISTS SAO_RUNTIME_AUDIT_FORBIDDEN_SHIP_FILES)
        if (EXISTS "${SAO_RUNTIME_AUDIT_SHIP_DIRECTORY}/${_sao_forbidden}")
            message(FATAL_ERROR
                "plaintext runtime artifact leaked into ship tree: ${_sao_forbidden}")
        endif()
    endforeach()
endif()

if (NOT DEFINED SAO_RUNTIME_AUDIT_DUMPBIN OR
    "${SAO_RUNTIME_AUDIT_DUMPBIN}" STREQUAL "" OR
    NOT EXISTS "${SAO_RUNTIME_AUDIT_DUMPBIN}")
    message(FATAL_ERROR "dumpbin is required for runtime PE audit")
endif()

set(_sao_runtime_encrypted_dependencies "")
set(_sao_runtime_input_names "")
set(_sao_runtime_payload_count 0)
set(_sao_runtime_payload_leaf "")
foreach (_sao_pe IN LISTS SAO_RUNTIME_AUDIT_PES)
    get_filename_component(_sao_input_name "${_sao_pe}" NAME)
    get_filename_component(_sao_input_extension "${_sao_pe}" EXT)
    string(TOLOWER "${_sao_input_name}" _sao_input_name_lower)
    string(TOLOWER "${_sao_input_extension}" _sao_input_extension_lower)
    list(FIND _sao_runtime_input_names
        "${_sao_input_name_lower}" _sao_runtime_input_name_index)
    if (NOT _sao_runtime_input_name_index EQUAL -1)
        message(FATAL_ERROR
            "runtime plaintext input basename is duplicated: ${_sao_input_name}")
    endif()
    list(APPEND _sao_runtime_input_names "${_sao_input_name_lower}")
    if (_sao_input_extension_lower STREQUAL ".dll")
        list(APPEND _sao_runtime_encrypted_dependencies
            "${_sao_input_name_lower}")
    elseif (_sao_input_extension_lower STREQUAL ".exe")
        math(EXPR _sao_runtime_payload_count
            "${_sao_runtime_payload_count} + 1")
        set(_sao_runtime_payload_leaf "${_sao_input_name}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _sao_runtime_encrypted_dependencies)
list(LENGTH _sao_runtime_encrypted_dependencies
    _sao_runtime_encrypted_dependency_count)
if (NOT _sao_runtime_payload_count EQUAL 1 OR
    NOT _sao_runtime_encrypted_dependency_count EQUAL 16 OR
    NOT "${_sao_runtime_payload_leaf}" STREQUAL
        "${SAO_RUNTIME_AUDIT_EXPECTED_PAYLOAD_LEAF}")
    message(FATAL_ERROR
        "runtime plaintext input set does not match the frozen payload/DLL layout")
endif()

set(_sao_runtime_system_dlls
    advapi32.dll bcrypt.dll bcryptprimitives.dll cfgmgr32.dll combase.dll comctl32.dll
    comdlg32.dll crypt32.dll d2d1.dll d3d11.dll dcomp.dll dbghelp.dll dwrite.dll
    dwmapi.dll dxgi.dll gdi32.dll hid.dll imagehlp.dll imm32.dll iphlpapi.dll
    kernel32.dll kernelbase.dll msvcrt.dll ntdll.dll normaliz.dll
    ole32.dll oleaut32.dll psapi.dll rpcrt4.dll sechost.dll secur32.dll setupapi.dll shell32.dll
    shcore.dll shlwapi.dll user32.dll userenv.dll uxtheme.dll version.dll winhttp.dll
    wininet.dll winmm.dll wintrust.dll windowsapp.dll ws2_32.dll wtsapi32.dll
    mswsock.dll nsi.dll ucrtbase.dll xaudio2_9.dll)

function(_sao_runtime_dependency_is_system dependency out_result)
    list(FIND _sao_runtime_system_dlls "${dependency}" _sao_system_index)
    if (NOT _sao_system_index EQUAL -1 OR
        dependency MATCHES "^(api-ms-win|ext-ms-win)-[a-z0-9-]+\\.dll$")
        set(${out_result} TRUE PARENT_SCOPE)
    else()
        set(${out_result} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_sao_runtime_audit_dependencies pe)
    execute_process(
        COMMAND "${SAO_RUNTIME_AUDIT_DUMPBIN}" /nologo /dependents "${pe}"
        RESULT_VARIABLE _sao_dependents_result
        OUTPUT_VARIABLE _sao_dependents_output
        ERROR_VARIABLE _sao_dependents_error)
    if (NOT _sao_dependents_result EQUAL 0)
        message(FATAL_ERROR
            "dumpbin /dependents failed for ${pe}: ${_sao_dependents_error}")
    endif()
    string(REPLACE "\r\n" "\n" _sao_dependents_output
        "${_sao_dependents_output}")
    string(REPLACE "\r" "\n" _sao_dependents_output
        "${_sao_dependents_output}")
    string(REPLACE "\n" ";" _sao_dependency_lines
        "${_sao_dependents_output}")
    foreach (_sao_dependency_line IN LISTS _sao_dependency_lines)
        string(STRIP "${_sao_dependency_line}" _sao_dependency)
        if (NOT _sao_dependency MATCHES "^[A-Za-z0-9_.+-]+\\.dll$")
            continue()
        endif()
        string(TOLOWER "${_sao_dependency}" _sao_dependency_lower)
        list(FIND _sao_runtime_encrypted_dependencies
            "${_sao_dependency_lower}" _sao_encrypted_index)
        if (NOT _sao_encrypted_index EQUAL -1)
            continue()
        endif()
        _sao_runtime_dependency_is_system(
            "${_sao_dependency_lower}" _sao_is_system)
        if (_sao_is_system)
            continue()
        endif()
        set(_sao_staged_dependency FALSE)
        foreach (_sao_dependency_root IN ITEMS
                "${SAO_RUNTIME_AUDIT_SHIP_DIRECTORY}/bin"
                "${SAO_RUNTIME_AUDIT_SHIP_DIRECTORY}/bin/runtime/helper")
            if (EXISTS "${_sao_dependency_root}/${_sao_dependency}" AND
                NOT IS_DIRECTORY "${_sao_dependency_root}/${_sao_dependency}")
                set(_sao_staged_dependency TRUE)
                break()
            endif()
        endforeach()
        if (_sao_staged_dependency)
            continue()
        endif()
        message(FATAL_ERROR
            "encrypted runtime PE has an unresolved app dependency: "
            "${pe} -> ${_sao_dependency}")
    endforeach()
endfunction()

set(_sao_checked 0)
foreach (_sao_pe IN LISTS SAO_RUNTIME_AUDIT_PES)
    if (NOT EXISTS "${_sao_pe}")
        message(FATAL_ERROR "runtime PE is missing before encryption: ${_sao_pe}")
    endif()
    get_filename_component(_sao_leaf "${_sao_pe}" NAME)
    get_filename_component(_sao_stem "${_sao_pe}" NAME_WE)
    get_filename_component(_sao_extension "${_sao_pe}" EXT)
    string(TOLOWER "${_sao_leaf}" _sao_leaf_lower)
    string(TOLOWER "${_sao_extension}" _sao_extension_lower)
    string(LENGTH "${_sao_stem}" _sao_stem_length)
    if (NOT "${_sao_leaf}" STREQUAL "${_sao_leaf_lower}" OR
        NOT _sao_stem_length EQUAL 16 OR
        NOT "${_sao_stem}" MATCHES "^[0-9a-f]+$" OR
        (NOT _sao_extension_lower STREQUAL ".exe" AND
         NOT _sao_extension_lower STREQUAL ".dll"))
        message(FATAL_ERROR
            "runtime PE does not have an opaque 16-hex basename: ${_sao_leaf}")
    endif()

    execute_process(
        COMMAND "${SAO_RUNTIME_AUDIT_DUMPBIN}" /nologo /headers /imports "${_sao_pe}"
        RESULT_VARIABLE _sao_dump_result
        OUTPUT_VARIABLE _sao_dump_output
        ERROR_VARIABLE _sao_dump_error)
    if (NOT _sao_dump_result EQUAL 0)
        message(FATAL_ERROR
            "dumpbin failed for ${_sao_pe}: ${_sao_dump_error}")
    endif()
    string(TOLOWER "${_sao_dump_output}" _sao_dump_lower)
    if (NOT _sao_dump_lower MATCHES "machine \\(x64\\)")
        message(FATAL_ERROR "runtime PE is not AMD64: ${_sao_pe}")
    endif()
    if (_sao_dump_lower MATCHES "sao_[a-z0-9_]+\\.dll")
        message(FATAL_ERROR
            "runtime PE still imports a descriptive first-party DLL: ${_sao_pe}")
    endif()
    if (_sao_dump_lower MATCHES "(vcruntime140d|msvcp140d|ucrtbased)\\.dll")
        message(FATAL_ERROR "runtime PE imports a Debug CRT: ${_sao_pe}")
    endif()
    _sao_runtime_audit_dependencies("${_sao_pe}")
    math(EXPR _sao_checked "${_sao_checked} + 1")
endforeach()

message(STATUS
    "Encrypted runtime bundle audit passed: ${_sao_checked} plaintext PE inputs, "
    "${_sao_bundle_size} authenticated bundle bytes")
