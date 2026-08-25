if (NOT DEFINED SAO_PROVIDER_CONFIG_SOURCE OR
    NOT DEFINED SAO_PROVIDER_CONFIG_DEST OR
    NOT DEFINED SAO_PROVIDER_CONFIG_CONFIG)
    message(FATAL_ERROR "Provider config staging arguments are incomplete")
endif()

if (EXISTS "${SAO_PROVIDER_CONFIG_SOURCE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SAO_PROVIDER_CONFIG_SOURCE}"
            "${SAO_PROVIDER_CONFIG_DEST}"
        RESULT_VARIABLE _sao_copy_result)
    if (NOT _sao_copy_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to copy SaoAuto.provider.json beside SaoAuto.exe")
    endif()
elseif (NOT SAO_PROVIDER_CONFIG_CONFIG STREQUAL "Debug")
    message(FATAL_ERROR
        "Product build requires launcher/config/SaoAuto.provider.json; "
        "create it before building ${SAO_PROVIDER_CONFIG_CONFIG}")
endif()