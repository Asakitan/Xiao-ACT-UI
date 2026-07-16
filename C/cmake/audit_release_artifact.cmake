if (NOT DEFINED SAO_AUDIT_BINARY OR NOT EXISTS "${SAO_AUDIT_BINARY}")
    message(FATAL_ERROR "SAO_AUDIT_BINARY must name an existing PE artifact")
endif()

if (NOT DEFINED SAO_AUDIT_DUMPBIN OR NOT EXISTS "${SAO_AUDIT_DUMPBIN}")
    message(FATAL_ERROR "dumpbin.exe is required for the release artifact audit")
endif()

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

sao_audit_dumpbin(/headers SAO_AUDIT_HEADERS)
sao_audit_dumpbin(/imports SAO_AUDIT_IMPORTS)
sao_audit_dumpbin(/exports SAO_AUDIT_EXPORTS)

if (SAO_AUDIT_EXPECT_HARDENING STREQUAL "ON")
    foreach (_sao_required IN ITEMS "Dynamic base" "High Entropy" "NX compatible" "Control Flow Guard")
        string(FIND "${SAO_AUDIT_HEADERS}" "${_sao_required}" _sao_match)
        if (_sao_match EQUAL -1)
            message(FATAL_ERROR "${SAO_AUDIT_BINARY} is missing required PE mitigation: ${_sao_required}")
        endif()
    endforeach()

    if (SAO_AUDIT_EXPECT_CET STREQUAL "ON")
        string(REGEX MATCH "[Cc][Ee][Tt][^\r\n]*[Cc]ompat" _sao_cet_match "${SAO_AUDIT_HEADERS}")
        if (NOT _sao_cet_match)
            message(FATAL_ERROR "${SAO_AUDIT_BINARY} is missing CETCOMPAT")
        endif()
    endif()
endif()

string(TOLOWER "${SAO_AUDIT_IMPORTS}" _sao_imports_lower)
string(FIND "${_sao_imports_lower}" "vcruntime140d.dll" _sao_debug_vcruntime)
string(FIND "${_sao_imports_lower}" "msvcp140d.dll" _sao_debug_msvcp)
if (NOT _sao_debug_vcruntime EQUAL -1 OR NOT _sao_debug_msvcp EQUAL -1)
    message(FATAL_ERROR "${SAO_AUDIT_BINARY} imports a debug C++ runtime")
endif()

string(FIND "${SAO_AUDIT_HEADERS}" ".rsrc" _sao_resource_section)
if (_sao_resource_section EQUAL -1)
    message(FATAL_ERROR "${SAO_AUDIT_BINARY} has no auditable resource section")
endif()

file(READ "${SAO_AUDIT_BINARY}" _sao_binary_hex HEX)
if (DEFINED SAO_AUDIT_SOURCE_ROOT AND NOT SAO_AUDIT_SOURCE_ROOT STREQUAL "")
    string(HEX "${SAO_AUDIT_SOURCE_ROOT}" _sao_source_root_hex)
    string(FIND "${_sao_binary_hex}" "${_sao_source_root_hex}" _sao_source_root_match)
    if (NOT _sao_source_root_match EQUAL -1)
        message(FATAL_ERROR "${SAO_AUDIT_BINARY} embeds the source-root path")
    endif()
endif()

set(_sao_forbidden_patterns
    "*.pdb"
    "*.ipdb"
    "*.iobj"
    "*.ilk"
    "*.lib"
    "*.exp"
    "*.map"
    "*.dmp"
    "*.tlog"
)
if (DEFINED SAO_AUDIT_SHIP_DIRECTORY AND EXISTS "${SAO_AUDIT_SHIP_DIRECTORY}")
    foreach (_sao_pattern IN LISTS _sao_forbidden_patterns)
        file(GLOB_RECURSE _sao_forbidden_files LIST_DIRECTORIES false
            "${SAO_AUDIT_SHIP_DIRECTORY}/${_sao_pattern}")
        if (_sao_forbidden_files)
            message(FATAL_ERROR "Ship directory contains diagnostic artifacts: ${_sao_forbidden_files}")
        endif()
    endforeach()
endif()

string(REGEX MATCHALL "[\r\n][ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+" _sao_export_rows "${SAO_AUDIT_EXPORTS}")
list(LENGTH _sao_export_rows SAO_AUDIT_EXPORT_COUNT)
message(STATUS "Release audit passed: ${SAO_AUDIT_BINARY}")
message(STATUS "  exports audited : ${SAO_AUDIT_EXPORT_COUNT}")
message(STATUS "  imports audited : no debug C++ runtime")
message(STATUS "  resources       : .rsrc present")
message(STATUS "  ship diagnostics: absent")