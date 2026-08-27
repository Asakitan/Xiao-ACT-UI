# Hardened post-link dispatcher.
# This helper is intentionally standalone; no production build target invokes
# it until a complete consumer and shipping contract exists.
# The parent build creates this step only when SAO_HARDENED_BUILD is enabled;
# the configuration guard below keeps multi-config Debug builds transform-free.

if (NOT DEFINED SAO_POST_LINK_MODE OR
    NOT DEFINED SAO_POST_LINK_CONFIG OR
    NOT DEFINED SAO_POST_LINK_TOOL OR
    NOT DEFINED SAO_POST_LINK_INPUT OR
    NOT DEFINED SAO_POST_LINK_OUTPUT)
    message(FATAL_ERROR "Incomplete hardened post-link invocation")
endif()

if (NOT SAO_POST_LINK_CONFIG STREQUAL "Release")
    return()
endif()

if (NOT EXISTS "${SAO_POST_LINK_TOOL}")
    message(FATAL_ERROR "Hardened post-link tool is missing: ${SAO_POST_LINK_TOOL}")
endif()
if (NOT EXISTS "${SAO_POST_LINK_INPUT}")
    message(FATAL_ERROR "Hardened post-link input is missing: ${SAO_POST_LINK_INPUT}")
endif()

get_filename_component(_sao_post_link_output_directory
    "${SAO_POST_LINK_OUTPUT}" DIRECTORY)
if (_sao_post_link_output_directory)
    file(MAKE_DIRECTORY "${_sao_post_link_output_directory}")
endif()

if (SAO_POST_LINK_MODE STREQUAL "obfuscate")
    if (NOT DEFINED SAO_POST_LINK_SEED OR
        NOT SAO_POST_LINK_SEED MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Hardened obfuscation requires a non-zero decimal seed")
    endif()
    execute_process(
        COMMAND "${SAO_POST_LINK_TOOL}"
            --input "${SAO_POST_LINK_INPUT}"
            --output "${SAO_POST_LINK_OUTPUT}"
            --passes strenc
            --seed "${SAO_POST_LINK_SEED}"
        RESULT_VARIABLE _sao_post_link_result
        OUTPUT_VARIABLE _sao_post_link_stdout
        ERROR_VARIABLE _sao_post_link_stderr)
elseif (SAO_POST_LINK_MODE STREQUAL "crypter")
    if ("$ENV{SAO_CRYPTER_KEY}" STREQUAL "")
        message(FATAL_ERROR
            "SAO_CRYPTER_KEY is required for registered native-loader payload targets")
    endif()
    execute_process(
        COMMAND "${SAO_POST_LINK_TOOL}"
            --format native-loader
            --input "${SAO_POST_LINK_INPUT}"
            --output "${SAO_POST_LINK_OUTPUT}"
            --key env
            --verify
        RESULT_VARIABLE _sao_post_link_result
        OUTPUT_VARIABLE _sao_post_link_stdout
        ERROR_VARIABLE _sao_post_link_stderr)
else()
    message(FATAL_ERROR "Unknown hardened post-link mode: ${SAO_POST_LINK_MODE}")
endif()

if (NOT _sao_post_link_result EQUAL 0)
    message(FATAL_ERROR
        "Hardened post-link ${SAO_POST_LINK_MODE} failed for ${SAO_POST_LINK_INPUT} "
        "with exit code ${_sao_post_link_result}\n"
        "stdout=${_sao_post_link_stdout}\n"
        "stderr=${_sao_post_link_stderr}")
endif()

message(STATUS
    "Hardened post-link ${SAO_POST_LINK_MODE}: ${SAO_POST_LINK_OUTPUT}")
