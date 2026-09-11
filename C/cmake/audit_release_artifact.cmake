if (NOT DEFINED SAO_AUDIT_SHIP_DIRECTORY OR
    NOT IS_DIRECTORY "${SAO_AUDIT_SHIP_DIRECTORY}")
    message(FATAL_ERROR
        "SAO_AUDIT_SHIP_DIRECTORY must name an existing clean install tree")
endif()

if (NOT DEFINED SAO_AUDIT_BINARY OR NOT EXISTS "${SAO_AUDIT_BINARY}")
    message(FATAL_ERROR "SAO_AUDIT_BINARY must name the staged SaoAuto PE")
endif()

if (NOT DEFINED SAO_AUDIT_EXPECTED_FILES OR
    "${SAO_AUDIT_EXPECTED_FILES}" STREQUAL "")
    message(FATAL_ERROR
        "SAO_AUDIT_EXPECTED_FILES must provide the canonical staged inventory")
endif()

if (NOT DEFINED SAO_AUDIT_PROJECT_OWNED_PES OR
    "${SAO_AUDIT_PROJECT_OWNED_PES}" STREQUAL "")
    message(FATAL_ERROR
        "SAO_AUDIT_PROJECT_OWNED_PES must provide explicit project-owned PE paths")
endif()

set(_sao_helper_binary
    "${SAO_AUDIT_SHIP_DIRECTORY}/bin/runtime/helper/WdiSvcHost.exe")
set(_sao_helper_identity "${_sao_helper_binary}.identity")
if (EXISTS "${_sao_helper_binary}" OR EXISTS "${_sao_helper_identity}")
    if (NOT EXISTS "${_sao_helper_binary}" OR NOT EXISTS "${_sao_helper_identity}")
        message(FATAL_ERROR "Staged helper executable and identity sidecar must both exist")
    endif()
    if (NOT DEFINED SAO_AUDIT_SOURCE_ROOT OR
        NOT IS_DIRECTORY "${SAO_AUDIT_SOURCE_ROOT}")
        message(FATAL_ERROR "SAO_AUDIT_SOURCE_ROOT is required for helper identity closure")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSAO_RT_IO_IDENTITY_VERIFY_ONLY=ON"
            "-DSAO_RT_IO_HELPER_BINARY=${_sao_helper_binary}"
            "-DSAO_RT_IO_HELPER_IDENTITY_INPUT=${_sao_helper_identity}"
            "-DSAO_RT_IO_ABI_HEADER=${SAO_AUDIT_SOURCE_ROOT}/platform/rt_io/include/sao/rt_io/abi.h"
            "-DSAO_RT_IO_PROTOCOL_HEADER=${SAO_AUDIT_SOURCE_ROOT}/platform/rt_io/include/sao/rt_io/protocol.h"
            "-DSAO_RT_IO_HELPER_BOOTSTRAP_HEADER=${SAO_AUDIT_SOURCE_ROOT}/platform/rt_io/include/sao/rt_io/helper_bootstrap.h"
            -P "${SAO_AUDIT_SOURCE_ROOT}/platform/rt_io/cmake/generate_helper_identity.cmake"
        RESULT_VARIABLE _sao_helper_identity_result
        OUTPUT_VARIABLE _sao_helper_identity_output
        ERROR_VARIABLE _sao_helper_identity_error)
    if (NOT _sao_helper_identity_result EQUAL 0)
        message(FATAL_ERROR
            "Staged helper identity closure failed: ${_sao_helper_identity_output}${_sao_helper_identity_error}")
    endif()
    message(STATUS "Staged helper protocol/ABI identity closure passed")
endif()

set(_sao_ship_bin_directory "${SAO_AUDIT_SHIP_DIRECTORY}/bin")
if (NOT IS_DIRECTORY "${_sao_ship_bin_directory}")
    message(FATAL_ERROR "Staged install tree is missing bin/: ${_sao_ship_bin_directory}")
endif()

set(_sao_expected_relative_files "")
foreach (_sao_expected_file IN LISTS SAO_AUDIT_EXPECTED_FILES)
    if (IS_ABSOLUTE "${_sao_expected_file}" OR
        "${_sao_expected_file}" MATCHES "(^|/)\\.\\.?(/|$)")
        message(FATAL_ERROR
            "Release inventory entry is not relative to the ship tree: ${_sao_expected_file}")
    endif()
    if (NOT EXISTS "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_expected_file}")
        message(FATAL_ERROR
            "Required staged release inventory entry is missing: ${_sao_expected_file}")
    endif()
    cmake_path(CONVERT "${_sao_expected_file}" TO_CMAKE_PATH_LIST
        _sao_expected_file_normalized NORMALIZE)
    list(APPEND _sao_expected_relative_files
        "${_sao_expected_file_normalized}")
endforeach()
list(REMOVE_DUPLICATES _sao_expected_relative_files)
list(SORT _sao_expected_relative_files)
set(SAO_AUDIT_EXPECTED_FILES "${_sao_expected_relative_files}")

foreach (_sao_project_owned_pe IN LISTS SAO_AUDIT_PROJECT_OWNED_PES)
    if (IS_ABSOLUTE "${_sao_project_owned_pe}" OR
        "${_sao_project_owned_pe}" MATCHES "(^|/)\.\.?(/|$)" OR
        NOT "${_sao_project_owned_pe}" MATCHES "\.(exe|dll)$" OR
        "${_sao_project_owned_pe}" MATCHES "(^|/)WebView2Loader\.dll$")
        message(FATAL_ERROR
            "Project-owned PE entry is not an allowed relative executable/DLL path: "
            "${_sao_project_owned_pe}")
    endif()
    list(FIND SAO_AUDIT_EXPECTED_FILES
        "${_sao_project_owned_pe}" _sao_project_owned_expected_index)
    if (_sao_project_owned_expected_index EQUAL -1)
        message(FATAL_ERROR
            "Project-owned PE is missing from the expected staged inventory: "
            "${_sao_project_owned_pe}")
    endif()
    if (NOT EXISTS "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_project_owned_pe}" OR
        IS_DIRECTORY "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_project_owned_pe}")
        message(FATAL_ERROR
            "Project-owned PE is missing from the staged ship tree: "
            "${_sao_project_owned_pe}")
    endif()
endforeach()

if (NOT DEFINED SAO_AUDIT_REQUIRED_DIRECTORIES OR "${SAO_AUDIT_REQUIRED_DIRECTORIES}" STREQUAL "")
    message(FATAL_ERROR "SAO_AUDIT_REQUIRED_DIRECTORIES must provide the staged directory contract")
endif()
foreach (_sao_required_directory IN LISTS SAO_AUDIT_REQUIRED_DIRECTORIES)
    if (NOT IS_DIRECTORY "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_required_directory}")
        message(FATAL_ERROR "Required staged release directory is missing: ${_sao_required_directory}")
    endif()
endforeach()

if (NOT DEFINED SAO_AUDIT_INSTALL_MANIFEST OR NOT EXISTS "${SAO_AUDIT_INSTALL_MANIFEST}")
    message(FATAL_ERROR "The ship install manifest is required: ${SAO_AUDIT_INSTALL_MANIFEST}")
endif()
if (NOT DEFINED SAO_AUDIT_PLUGIN_ROOT OR "${SAO_AUDIT_PLUGIN_ROOT}" STREQUAL "")
    message(FATAL_ERROR "SAO_AUDIT_PLUGIN_ROOT must name the staged plugin root")
endif()
cmake_path(CONVERT "${SAO_AUDIT_PLUGIN_ROOT}" TO_CMAKE_PATH_LIST
    _sao_audit_plugin_root NORMALIZE)
if (IS_ABSOLUTE "${_sao_audit_plugin_root}" OR
    NOT IS_DIRECTORY "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_audit_plugin_root}")
    message(FATAL_ERROR
        "The staged plugin root is missing or not relative to the ship tree: "
        "${SAO_AUDIT_PLUGIN_ROOT}")
endif()
if (NOT DEFINED SAO_AUDIT_PROVIDER_CONFIG OR NOT EXISTS "${SAO_AUDIT_PROVIDER_CONFIG}")
    message(FATAL_ERROR "The staged provider config is required: ${SAO_AUDIT_PROVIDER_CONFIG}")
endif()
file(READ "${SAO_AUDIT_PROVIDER_CONFIG}" _sao_provider_config_text)
string(JSON _sao_provider_roots_type
    ERROR_VARIABLE _sao_provider_json_error
    TYPE "${_sao_provider_config_text}" plugins roots)
if (_sao_provider_json_error OR NOT _sao_provider_roots_type STREQUAL "ARRAY")
    message(FATAL_ERROR
        "Staged provider config must contain a JSON array at plugins.roots"
        " (error=${_sao_provider_json_error})")
endif()
string(JSON _sao_provider_roots_length
    ERROR_VARIABLE _sao_provider_json_error
    LENGTH "${_sao_provider_config_text}" plugins roots)
if (_sao_provider_json_error OR NOT _sao_provider_roots_length EQUAL 1)
    message(FATAL_ERROR
        "Staged provider config plugins.roots must contain exactly one entry")
endif()
string(JSON _sao_provider_root
    ERROR_VARIABLE _sao_provider_json_error
    GET "${_sao_provider_config_text}" plugins roots 0)
if (_sao_provider_json_error OR NOT _sao_provider_root STREQUAL "plugins")
    message(FATAL_ERROR
        "Staged provider config plugins.roots must be exactly [\"plugins\"]")
endif()

file(STRINGS "${SAO_AUDIT_INSTALL_MANIFEST}" _sao_install_manifest_entries)
if (NOT _sao_install_manifest_entries)
    message(FATAL_ERROR "The ship install manifest is empty: ${SAO_AUDIT_INSTALL_MANIFEST}")
endif()
set(_sao_manifest_relative_files "")
foreach (_sao_manifest_entry IN LISTS _sao_install_manifest_entries)
    string(STRIP "${_sao_manifest_entry}" _sao_manifest_entry)
    file(RELATIVE_PATH _sao_manifest_relative "${SAO_AUDIT_SHIP_DIRECTORY}" "${_sao_manifest_entry}")
    cmake_path(CONVERT "${_sao_manifest_relative}" TO_CMAKE_PATH_LIST _sao_manifest_relative NORMALIZE)
    if (_sao_manifest_relative MATCHES "(^|/)\\.\\.(/|$)" OR NOT EXISTS "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_manifest_relative}")
        message(FATAL_ERROR "Invalid or missing ship install manifest entry: ${_sao_manifest_entry}")
    endif()
    list(APPEND _sao_manifest_relative_files "${_sao_manifest_relative}")
endforeach()
list(REMOVE_DUPLICATES _sao_manifest_relative_files)
list(SORT _sao_manifest_relative_files)

set(_sao_expected_not_manifest "")
foreach (_sao_expected_file IN LISTS SAO_AUDIT_EXPECTED_FILES)
    list(FIND _sao_manifest_relative_files "${_sao_expected_file}"
        _sao_expected_manifest_index)
    if (_sao_expected_manifest_index EQUAL -1)
        list(APPEND _sao_expected_not_manifest "${_sao_expected_file}")
    endif()
endforeach()
set(_sao_manifest_not_expected "")
foreach (_sao_manifest_file IN LISTS _sao_manifest_relative_files)
    list(FIND SAO_AUDIT_EXPECTED_FILES "${_sao_manifest_file}"
        _sao_manifest_expected_index)
    if (_sao_manifest_expected_index EQUAL -1)
        list(APPEND _sao_manifest_not_expected "${_sao_manifest_file}")
    endif()
endforeach()
if (_sao_expected_not_manifest OR _sao_manifest_not_expected)
    message(FATAL_ERROR
        "Canonical release inventory and install manifest differ. "
        "expected-not-manifest=[${_sao_expected_not_manifest}] "
        "manifest-not-expected=[${_sao_manifest_not_expected}]")
endif()

foreach (_sao_project_owned_pe IN LISTS SAO_AUDIT_PROJECT_OWNED_PES)
    list(FIND _sao_manifest_relative_files
        "${_sao_project_owned_pe}" _sao_project_owned_manifest_index)
    if (_sao_project_owned_manifest_index EQUAL -1)
        message(FATAL_ERROR
            "Project-owned PE is missing from the ship install manifest: "
            "${_sao_project_owned_pe}")
    endif()
endforeach()

file(GLOB_RECURSE _sao_forbidden_runtime_manifest_files LIST_DIRECTORIES false
    "${SAO_AUDIT_SHIP_DIRECTORY}/share/sao/plugin_runtimes/*"
    "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_audit_plugin_root}/runtimes/manifest.json"
    "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_audit_plugin_root}/runtimes/build-time-manifest.json")
if (_sao_forbidden_runtime_manifest_files)
    message(FATAL_ERROR "Build-time runtime metadata must not be installed or shipped: ${_sao_forbidden_runtime_manifest_files}")
endif()

if (NOT DEFINED SAO_AUDIT_DUMPBIN OR NOT EXISTS "${SAO_AUDIT_DUMPBIN}")
    message(FATAL_ERROR "dumpbin.exe is required for the release artifact audit")
endif()

# Audit runs over every PE artifact staged under the ship directory, not
# just SaoAuto.exe.  Each helper reads the current SAO_AUDIT_BINARY, so
# the driver loop just rebinds it before invoking the individual checks.
function(sao_audit_dumpbin mode output_var)
    execute_process(
        COMMAND "${SAO_AUDIT_DUMPBIN}" "${mode}" "${SAO_AUDIT_BINARY}"
        RESULT_VARIABLE _sao_result
        OUTPUT_VARIABLE _sao_output
        ERROR_VARIABLE _sao_error
    )
    if (NOT _sao_result EQUAL 0)
        message(FATAL_ERROR "dumpbin ${mode} failed: ${_sao_error}")
    endif()
    set(${output_var} "${_sao_output}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Denylists
#
# Debug C++ runtime imports must never appear in a ship artifact.  We reject
# both the traditional Debug CRT DLLs (vcruntime140d / msvcp140d), the
# universal CRT Debug DLL (ucrtbased) and the api-ms-win-*d.dll debug
# forwarders that a Debug-configured toolchain silently pulls in.
# ---------------------------------------------------------------------------
set(_SAO_AUDIT_DEBUG_CRT_LITERALS
    "vcruntime140d.dll"
    "vcruntime140_1d.dll"
    "msvcp140d.dll"
    "msvcp140_1d.dll"
    "msvcp140_2d.dll"
    "ucrtbased.dll"
    "concrt140d.dll"
    "mfc140d.dll"
    "mfc140ud.dll"
)

# api-ms-win-*d.dll: the universal CRT debug forwarders.  Match via regex
# because there are many forwarder DLLs with numeric revisions.
set(_SAO_AUDIT_DEBUG_CRT_REGEXES
    "api-ms-win-[a-z0-9-]+d-[0-9]+-[0-9]+\\.dll"
)

# Plaintext sensitive-string denylist.  The regexes intentionally match
# concrete infrastructure hints (private endpoints, API keys, credential
# tokens) rather than every URL — the hardened build has legitimate use
# for public HTTPS URLs (docs, freetier endpoints).  Any match fails the
# build so the operator can either scrub the string or wrap it in
# SAO_ENC_STR() at compile time.
set(_SAO_AUDIT_SENSITIVE_STRING_REGEXES
    # Private / internal endpoints and hostnames.
    "https?://[a-z0-9.-]*\\.internal\\.[a-z0-9.-]+"
    "https?://[a-z0-9.-]*\\.corp\\.[a-z0-9.-]+"
    "https?://[a-z0-9.-]*\\.private\\.[a-z0-9.-]+"
    "https?://[a-z0-9.-]*\\.local\\.[a-z0-9.-]+"
    "https?://[a-z0-9.-]*\\.lan\\.[a-z0-9.-]+"
    "https?://[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+(:[0-9]+)?/"
    # API key / bearer token prefixes commonly leaked in source or config.
    "sk-[A-Za-z0-9_-]+"
    "AKIA[0-9A-Z]{16}"
    "ghp_[A-Za-z0-9]{20,}"
    "gho_[A-Za-z0-9]{20,}"
    "ghs_[A-Za-z0-9]{20,}"
    "xox[abpr]-[A-Za-z0-9-]+"
    "AIza[0-9A-Za-z_-]{20,}"
    # Google service-account private key markers.
    "-----BEGIN [A-Z ]*PRIVATE KEY-----"
    "-----BEGIN OPENSSH PRIVATE KEY-----"
    "-----BEGIN RSA PRIVATE KEY-----"
    "-----BEGIN EC PRIVATE KEY-----"
    "-----BEGIN DSA PRIVATE KEY-----"
    "-----BEGIN PGP PRIVATE KEY BLOCK-----"
)

set(_sao_forbidden_ship_patterns
    "*.pdb"
    "*.ipdb"
    "*.iobj"
    "*.ilk"
    "*.lib"
    "*.exp"
    "*.map"
    "*.dmp"
    "*.tlog"
    "*.saoobf"
    "*.bin.enc"
    "*.wrapped.*"
)

function(sao_audit_pe binary_path project_owned)
    set(SAO_AUDIT_BINARY "${binary_path}")
    sao_audit_dumpbin(/headers SAO_AUDIT_HEADERS)
    sao_audit_dumpbin(/imports SAO_AUDIT_IMPORTS)
    sao_audit_dumpbin(/exports SAO_AUDIT_EXPORTS)

    get_filename_component(_sao_binary_extension "${binary_path}" EXT)
    string(TOLOWER "${_sao_binary_extension}" _sao_binary_extension)

    if (project_owned)
        string(REPLACE "\r\n" "\n" _sao_headers_lines "${SAO_AUDIT_HEADERS}")
        string(REPLACE "\r" "\n" _sao_headers_lines "${_sao_headers_lines}")
        string(REPLACE "\n" ";" _sao_headers_lines "${_sao_headers_lines}")
        set(_sao_certificate_directory_lines "")
        foreach (_sao_headers_line IN LISTS _sao_headers_lines)
            if (_sao_headers_line MATCHES
                "Certificate[ \t]+Table|Certificates[ \t]+Directory")
                list(APPEND _sao_certificate_directory_lines "${_sao_headers_line}")
            endif()
        endforeach()
        if (NOT _sao_certificate_directory_lines)
            message(FATAL_ERROR
                "Project-owned release PE is missing a Certificate Table or "
                "Certificates Directory headers line: ${binary_path}")
        endif()
        foreach (_sao_certificate_directory_line IN LISTS _sao_certificate_directory_lines)
            if (NOT _sao_certificate_directory_line MATCHES
                "^[ \t]*[0-9A-Fa-f]+[ \t]+\[[ \t]*0[ \t]*\][ \t]+RVA[ \t]+\[[ \t]*size[ \t]*\][ \t]+of[ \t]+(Certificate[ \t]+Table|Certificates[ \t]+Directory)[ \t]*$")
                message(FATAL_ERROR
                    "Project-owned release PE must have a zero-size Certificate Table or "
                    "Certificates Directory: ${binary_path}: ${_sao_certificate_directory_line}")
            endif()
        endforeach()
        message(STATUS "${binary_path}: project-owned PE certificate table is empty")
    else()
        message(STATUS
            "${binary_path}: certificate table not constrained (third-party/runtime/driver artifact)")
    endif()

    if (SAO_AUDIT_EXPECT_HARDENING STREQUAL "ON")
        foreach (_sao_required IN ITEMS
                "Dynamic base"
                "High Entropy"
                "NX compatible"
                "Control Flow Guard")
            string(FIND "${SAO_AUDIT_HEADERS}" "${_sao_required}" _sao_match)
            if (_sao_match EQUAL -1)
                message(FATAL_ERROR
                    "${binary_path} is missing required PE mitigation: "
                    "${_sao_required}")
            endif()
        endforeach()

        if (SAO_AUDIT_EXPECT_CET STREQUAL "ON" AND
            NOT _sao_binary_extension STREQUAL ".sys")
            string(REGEX MATCH "[Cc][Ee][Tt][^\r\n]*[Cc]ompat"
                _sao_cet_match "${SAO_AUDIT_HEADERS}")
            if (NOT _sao_cet_match)
                message(FATAL_ERROR "${binary_path} is missing CETCOMPAT")
            endif()
        endif()
    endif()

    string(TOLOWER "${SAO_AUDIT_IMPORTS}" _sao_imports_lower)
    foreach (_sao_debug_dll IN LISTS _SAO_AUDIT_DEBUG_CRT_LITERALS)
        string(FIND "${_sao_imports_lower}" "${_sao_debug_dll}" _sao_debug_match)
        if (NOT _sao_debug_match EQUAL -1)
            message(FATAL_ERROR
                "${binary_path} imports the debug CRT DLL '${_sao_debug_dll}'")
        endif()
    endforeach()
    foreach (_sao_debug_regex IN LISTS _SAO_AUDIT_DEBUG_CRT_REGEXES)
        string(REGEX MATCH "${_sao_debug_regex}" _sao_debug_regex_match
            "${_sao_imports_lower}")
        if (_sao_debug_regex_match)
            message(FATAL_ERROR
                "${binary_path} imports a debug forwarder DLL matching "
                "'${_sao_debug_regex}': '${_sao_debug_regex_match}'")
        endif()
    endforeach()

    string(FIND "${SAO_AUDIT_HEADERS}" ".rsrc" _sao_resource_section)
    if (_sao_resource_section EQUAL -1)
        message(STATUS "${binary_path}: no .rsrc section (dependency DLL / import lib)")
    endif()

    file(READ "${binary_path}" _sao_binary_hex HEX)
    if (DEFINED SAO_AUDIT_SOURCE_ROOT AND NOT SAO_AUDIT_SOURCE_ROOT STREQUAL "")
        string(HEX "${SAO_AUDIT_SOURCE_ROOT}" _sao_source_root_hex)
        string(FIND "${_sao_binary_hex}" "${_sao_source_root_hex}"
            _sao_source_root_match)
        if (NOT _sao_source_root_match EQUAL -1)
            message(FATAL_ERROR
                "${binary_path} embeds the source-root path")
        endif()
    endif()

    # Plaintext sensitive-string scan.  file(STRINGS) already handles the
    # ASCII portion of a PE at the printable-character granularity — big
    # LIMIT_COUNT so long .rdata sections in shell/crypter-wrapped binaries
    # get inspected in one pass.  We check each string against the denylist
    # in BOTH its original case and lowercased form: API-key / PEM prefixes
    # (AKIA, AIza, "-----BEGIN … PRIVATE KEY-----") have canonical uppercase
    # spelling and only match the original; URLs and hostnames may appear in
    # arbitrary case and match the lowercase pass.  CMake regexes are always
    # case-sensitive so this two-pass approach is the correct portable fix.
    file(STRINGS "${binary_path}" _sao_binary_strings
        LIMIT_COUNT 200000
        LENGTH_MINIMUM 8
        ENCODING UTF-8)
    foreach (_sao_binary_string IN LISTS _sao_binary_strings)
        string(TOLOWER "${_sao_binary_string}" _sao_binary_string_lower)
        foreach (_sao_denylist_regex IN LISTS _SAO_AUDIT_SENSITIVE_STRING_REGEXES)
            string(REGEX MATCH "${_sao_denylist_regex}" _sao_sensitive_match
                "${_sao_binary_string}")
            if (NOT _sao_sensitive_match)
                string(REGEX MATCH "${_sao_denylist_regex}" _sao_sensitive_match
                    "${_sao_binary_string_lower}")
            endif()
            if (_sao_sensitive_match)
                message(FATAL_ERROR
                    "${binary_path} contains a plaintext sensitive string "
                    "matching '${_sao_denylist_regex}': "
                    "'${_sao_sensitive_match}'.  Scrub it or wrap it with "
                    "SAO_ENC_STR() so it is never emitted in cleartext.")
            endif()
        endforeach()
    endforeach()

    string(REGEX MATCHALL "[\r\n][ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+"
        _sao_export_rows "${SAO_AUDIT_EXPORTS}")
    list(LENGTH _sao_export_rows _sao_export_count)
    message(STATUS "Release audit passed: ${binary_path}")
    message(STATUS "  exports audited : ${_sao_export_count}")
    message(STATUS "  imports audited : no debug CRT / forwarder")
    message(STATUS "  strings audited : no denylisted plaintext")
endfunction()

set(_SAO_WINDOWS_SYSTEM_DLLS
    advapi32.dll bcrypt.dll bcryptprimitives.dll cfgmgr32.dll combase.dll comctl32.dll
    comdlg32.dll crypt32.dll d2d1.dll d3d11.dll dcomp.dll dbghelp.dll dwrite.dll
    dwmapi.dll dxgi.dll gdi32.dll hid.dll imagehlp.dll imm32.dll iphlpapi.dll
    kernel32.dll kernelbase.dll msvcrt.dll ntdll.dll
    ole32.dll oleaut32.dll psapi.dll rpcrt4.dll sechost.dll setupapi.dll shell32.dll
    shcore.dll shlwapi.dll user32.dll userenv.dll uxtheme.dll version.dll winhttp.dll
    wininet.dll winmm.dll wintrust.dll windowsapp.dll ws2_32.dll wtsapi32.dll
    normaliz.dll mswsock.dll nsi.dll ucrtbase.dll xaudio2_9.dll)

function(sao_audit_is_system_dependency dependency out_is_system)
    string(TOLOWER "${dependency}" _sao_dependency)
    list(FIND _SAO_WINDOWS_SYSTEM_DLLS "${_sao_dependency}" _sao_system_index)
    if (NOT _sao_system_index EQUAL -1 OR
        _sao_dependency MATCHES "^(api-ms-win|ext-ms-win)-[a-z0-9-]+\\.dll$")
        set(${out_is_system} TRUE PARENT_SCOPE)
    else()
        set(${out_is_system} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sao_audit_find_staged_dependency binary_path dependency out_path)
    get_filename_component(_sao_binary_directory "${binary_path}" DIRECTORY)
    set(_sao_match "")
    set(_sao_candidate "${_sao_binary_directory}/${dependency}")
    if (EXISTS "${_sao_candidate}" AND NOT IS_DIRECTORY "${_sao_candidate}")
        set(_sao_match "${_sao_candidate}")
    endif()
    set(${out_path} "${_sao_match}" PARENT_SCOPE)
endfunction()

function(sao_audit_dependency_closure binary_path parent_chain)
    string(TOLOWER "${binary_path}" _sao_binary_key)
    list(FIND _SAO_AUDIT_DEPENDENCY_VISITED "${_sao_binary_key}" _sao_seen)
    if (NOT _sao_seen EQUAL -1)
        return()
    endif()
    list(APPEND _SAO_AUDIT_DEPENDENCY_VISITED "${_sao_binary_key}")
    set(_SAO_AUDIT_DEPENDENCY_VISITED
        "${_SAO_AUDIT_DEPENDENCY_VISITED}" PARENT_SCOPE)

    set(SAO_AUDIT_BINARY "${binary_path}")
    sao_audit_dumpbin(/DEPENDENTS SAO_AUDIT_DEPENDENTS)
    string(REPLACE "\r\n" "\n" _sao_dependency_lines "${SAO_AUDIT_DEPENDENTS}")
    string(REPLACE "\r" "\n" _sao_dependency_lines "${_sao_dependency_lines}")
    string(REPLACE "\n" ";" _sao_dependency_lines "${_sao_dependency_lines}")
    set(_sao_dependency_rows "")
    foreach (_sao_dependency_line IN LISTS _sao_dependency_lines)
        string(STRIP "${_sao_dependency_line}" _sao_dependency_line)
        if (_sao_dependency_line MATCHES "^[A-Za-z0-9_.+-]+\\.dll$")
            list(APPEND _sao_dependency_rows "${_sao_dependency_line}")
        endif()
    endforeach()
    foreach (_sao_dependency_row IN LISTS _sao_dependency_rows)
        string(STRIP "${_sao_dependency_row}" _sao_dependency)
        string(TOLOWER "${_sao_dependency}" _sao_dependency_lower)
        sao_audit_find_staged_dependency(
            "${binary_path}" "${_sao_dependency_lower}"
            _sao_staged_dependency)
        if (_sao_staged_dependency)
            sao_audit_dependency_closure(
                "${_sao_staged_dependency}"
                "${parent_chain} -> ${_sao_dependency}")
            continue()
        endif()
        sao_audit_is_system_dependency(
            "${_sao_dependency_lower}" _sao_is_system_dependency)
        if (_sao_is_system_dependency)
            continue()
        endif()
        message(FATAL_ERROR
            "Unresolved non-system app dependency: ${parent_chain} -> ${_sao_dependency}")
    endforeach()
endfunction()
# ---------------------------------------------------------------------------
# Ship-directory sweep
#
# Discover every PE binary in the clean ship stage and audit each one.  The
# build tree is deliberately never consulted as a fallback.
# ---------------------------------------------------------------------------
set(_sao_ship_pe_binaries "")
if (DEFINED SAO_AUDIT_SHIP_DIRECTORY AND EXISTS "${SAO_AUDIT_SHIP_DIRECTORY}")
    foreach (_sao_pattern IN LISTS _sao_forbidden_ship_patterns)
        file(GLOB_RECURSE _sao_forbidden_files LIST_DIRECTORIES false
            "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_pattern}")
        if (_sao_forbidden_files)
            message(FATAL_ERROR
                "Ship directory contains diagnostic artifacts: "
                "${_sao_forbidden_files}")
        endif()
    endforeach()

    file(GLOB_RECURSE _sao_ship_pe_binaries LIST_DIRECTORIES false
        "${SAO_AUDIT_SHIP_DIRECTORY}/*.exe"
        "${SAO_AUDIT_SHIP_DIRECTORY}/*.dll"
        "${SAO_AUDIT_SHIP_DIRECTORY}/*.sys"
    )
endif()

if (NOT _sao_ship_pe_binaries)
    message(FATAL_ERROR
        "Clean ship directory contains no staged PE artifacts: ${SAO_AUDIT_SHIP_DIRECTORY}")
endif()

list(FIND _sao_ship_pe_binaries "${SAO_AUDIT_BINARY}" _sao_launcher_index)
if (_sao_launcher_index EQUAL -1)
    message(FATAL_ERROR
        "Canonical SaoAuto artifact is not present in the staged PE inventory: "
        "${SAO_AUDIT_BINARY}")
endif()

list(REMOVE_DUPLICATES _sao_ship_pe_binaries)
list(SORT _sao_ship_pe_binaries)

file(RELATIVE_PATH _sao_audit_binary_relative
    "${SAO_AUDIT_SHIP_DIRECTORY}" "${SAO_AUDIT_BINARY}")
cmake_path(CONVERT "${_sao_audit_binary_relative}" TO_CMAKE_PATH_LIST
    _sao_audit_binary_relative NORMALIZE)
list(FIND SAO_AUDIT_PROJECT_OWNED_PES
    "${_sao_audit_binary_relative}" _sao_audit_binary_project_owned_index)
if (_sao_audit_binary_project_owned_index EQUAL -1)
    message(FATAL_ERROR
        "Canonical SaoAuto artifact is not listed as project-owned: "
        "${_sao_audit_binary_relative}")
endif()

set(_sao_ship_pe_relative_paths)
foreach (_sao_ship_pe IN LISTS _sao_ship_pe_binaries)
    file(RELATIVE_PATH _sao_ship_pe_relative
        "${SAO_AUDIT_SHIP_DIRECTORY}" "${_sao_ship_pe}")
    cmake_path(CONVERT "${_sao_ship_pe_relative}" TO_CMAKE_PATH_LIST
        _sao_ship_pe_relative NORMALIZE)
    list(FIND SAO_AUDIT_EXPECTED_FILES
        "${_sao_ship_pe_relative}" _sao_expected_pe_index)
    if (_sao_expected_pe_index EQUAL -1)
        message(FATAL_ERROR
            "Ship directory PE artifact is missing from the expected staged inventory: "
            "${_sao_ship_pe_relative}")
    endif()
    list(FIND _sao_manifest_relative_files
        "${_sao_ship_pe_relative}" _sao_manifest_pe_index)
    if (_sao_manifest_pe_index EQUAL -1)
        message(FATAL_ERROR
            "Ship directory PE artifact is missing from the install manifest: "
            "${_sao_ship_pe_relative}")
    endif()
    list(APPEND _sao_ship_pe_relative_paths "${_sao_ship_pe_relative}")
endforeach()

foreach (_sao_project_owned_pe IN LISTS SAO_AUDIT_PROJECT_OWNED_PES)
    list(FIND _sao_ship_pe_relative_paths
        "${_sao_project_owned_pe}" _sao_project_owned_staged_index)
    if (_sao_project_owned_staged_index EQUAL -1)
        message(FATAL_ERROR
            "Project-owned PE is not present in the staged PE inventory: "
            "${_sao_project_owned_pe}")
    endif()
endforeach()

file(GLOB_RECURSE _sao_ship_regular_files LIST_DIRECTORIES false
    "${SAO_AUDIT_SHIP_DIRECTORY}/*")
set(_sao_ship_relative_files "")
foreach (_sao_ship_file IN LISTS _sao_ship_regular_files)
    file(RELATIVE_PATH _sao_ship_file_relative "${SAO_AUDIT_SHIP_DIRECTORY}" "${_sao_ship_file}")
    cmake_path(CONVERT "${_sao_ship_file_relative}" TO_CMAKE_PATH_LIST _sao_ship_file_relative NORMALIZE)
    list(APPEND _sao_ship_relative_files "${_sao_ship_file_relative}")
    list(FIND _sao_manifest_relative_files "${_sao_ship_file_relative}" _sao_manifest_file_index)
    if (_sao_manifest_file_index EQUAL -1)
        message(FATAL_ERROR "Ship directory contains an unexpected file outside install manifest: ${_sao_ship_file_relative}")
    endif()
endforeach()
foreach (_sao_manifest_relative IN LISTS _sao_manifest_relative_files)
    if (NOT EXISTS "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_manifest_relative}")
        message(FATAL_ERROR "Install manifest contains a non-staged file: ${_sao_manifest_relative}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _sao_ship_relative_files)
list(SORT _sao_ship_relative_files)
set(_sao_expected_not_ship "")
foreach (_sao_expected_file IN LISTS SAO_AUDIT_EXPECTED_FILES)
    list(FIND _sao_ship_relative_files "${_sao_expected_file}"
        _sao_expected_ship_index)
    if (_sao_expected_ship_index EQUAL -1)
        list(APPEND _sao_expected_not_ship "${_sao_expected_file}")
    endif()
endforeach()
set(_sao_ship_not_expected "")
foreach (_sao_ship_file IN LISTS _sao_ship_relative_files)
    list(FIND SAO_AUDIT_EXPECTED_FILES "${_sao_ship_file}"
        _sao_ship_expected_index)
    if (_sao_ship_expected_index EQUAL -1)
        list(APPEND _sao_ship_not_expected "${_sao_ship_file}")
    endif()
endforeach()
if (_sao_expected_not_ship OR _sao_ship_not_expected)
    message(FATAL_ERROR
        "Canonical release inventory and ship tree differ. "
        "expected-not-ship=[${_sao_expected_not_ship}] "
        "ship-not-expected=[${_sao_ship_not_expected}]")
endif()

set(_sao_pe_index 0)
list(LENGTH _sao_ship_pe_binaries _sao_pe_total)
message(STATUS "Release audit sweeping ${_sao_pe_total} PE binary(ies)")
foreach (_sao_ship_pe IN LISTS _sao_ship_pe_binaries)
    math(EXPR _sao_pe_index "${_sao_pe_index} + 1")
    message(STATUS "  [${_sao_pe_index}/${_sao_pe_total}] ${_sao_ship_pe}")
    file(RELATIVE_PATH _sao_ship_pe_relative
        "${SAO_AUDIT_SHIP_DIRECTORY}" "${_sao_ship_pe}")
    cmake_path(CONVERT "${_sao_ship_pe_relative}" TO_CMAKE_PATH_LIST
        _sao_ship_pe_relative NORMALIZE)
    list(FIND SAO_AUDIT_PROJECT_OWNED_PES
        "${_sao_ship_pe_relative}" _sao_project_owned_index)
    if (_sao_project_owned_index EQUAL -1)
        set(_sao_project_owned FALSE)
    else()
        set(_sao_project_owned TRUE)
    endif()
    sao_audit_pe("${_sao_ship_pe}" "${_sao_project_owned}")
endforeach()

set(_SAO_AUDIT_DEPENDENCY_VISITED "")
foreach (_sao_ship_pe IN LISTS _sao_ship_pe_binaries)
    sao_audit_dependency_closure("${_sao_ship_pe}" "${_sao_ship_pe}")
endforeach()
message(STATUS "Release audit passed for all ${_sao_pe_total} ship PE(s)")
list(LENGTH _sao_manifest_relative_files _sao_manifest_file_total)
message(STATUS "  exact install inventory: ${_sao_manifest_file_total} file(s)")
message(STATUS "  ship diagnostics: absent")
