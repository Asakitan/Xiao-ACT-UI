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

set(_sao_ship_bin_directory "${SAO_AUDIT_SHIP_DIRECTORY}/bin")
if (NOT IS_DIRECTORY "${_sao_ship_bin_directory}")
    message(FATAL_ERROR "Staged install tree is missing bin/: ${_sao_ship_bin_directory}")
endif()

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
endforeach()

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

function(sao_audit_pe binary_path)
    set(SAO_AUDIT_BINARY "${binary_path}")
    sao_audit_dumpbin(/headers SAO_AUDIT_HEADERS)
    sao_audit_dumpbin(/imports SAO_AUDIT_IMPORTS)
    sao_audit_dumpbin(/exports SAO_AUDIT_EXPORTS)

    get_filename_component(_sao_binary_extension "${binary_path}" EXT)
    string(TOLOWER "${_sao_binary_extension}" _sao_binary_extension)

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
    normaliz.dll mswsock.dll nsi.dll ucrtbase.dll)

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
    string(REGEX MATCHALL "[ \\t]+[A-Za-z0-9_.+\\-]+\\.dll" _sao_dependency_rows
        "${SAO_AUDIT_DEPENDENTS}")
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

foreach (_sao_ship_pe IN LISTS _sao_ship_pe_binaries)
    file(RELATIVE_PATH _sao_ship_pe_relative
        "${SAO_AUDIT_SHIP_DIRECTORY}" "${_sao_ship_pe}")
    cmake_path(CONVERT "${_sao_ship_pe_relative}" TO_CMAKE_PATH_LIST
        _sao_ship_pe_relative NORMALIZE)
    list(FIND SAO_AUDIT_EXPECTED_FILES
        "${_sao_ship_pe_relative}" _sao_expected_pe_index)
    if (_sao_expected_pe_index EQUAL -1)
        message(FATAL_ERROR
            "Ship directory contains an unexpected PE artifact: "
            "${_sao_ship_pe_relative}")
    endif()
endforeach()

set(_sao_pe_index 0)
list(LENGTH _sao_ship_pe_binaries _sao_pe_total)
message(STATUS "Release audit sweeping ${_sao_pe_total} PE binary(ies)")
foreach (_sao_ship_pe IN LISTS _sao_ship_pe_binaries)
    math(EXPR _sao_pe_index "${_sao_pe_index} + 1")
    message(STATUS "  [${_sao_pe_index}/${_sao_pe_total}] ${_sao_ship_pe}")
    sao_audit_pe("${_sao_ship_pe}")
endforeach()

set(_SAO_AUDIT_DEPENDENCY_VISITED "")
foreach (_sao_ship_pe IN LISTS _sao_ship_pe_binaries)
    sao_audit_dependency_closure("${_sao_ship_pe}" "${_sao_ship_pe}")
endforeach()
message(STATUS "Release audit passed for all ${_sao_pe_total} ship PE(s)")
message(STATUS "  ship diagnostics: absent")