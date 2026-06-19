set(SLICER_LINUX_RUNTIME_COMMON_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeConfig.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcProtocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcClient.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeEventPump.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeForwarderState.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeForwarderExports.cpp
)

get_filename_component(SLICER_LINUX_RUNTIME_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(SLICER_LINUX_RUNTIME_LINUX_HOST_DIR "${SLICER_LINUX_RUNTIME_REPO_ROOT}/tools/slicer_linux_runtime_host")
set(SLICER_LINUX_RUNTIME_INCLUDE_DIRS
    ${SLICER_LINUX_RUNTIME_REPO_ROOT}
    ${SLICER_LINUX_RUNTIME_REPO_ROOT}/src
    ${SLICER_LINUX_RUNTIME_REPO_ROOT}/deps_src
)

if (WIN32)
    add_library(slicer_linux_runtime SHARED
        ${SLICER_LINUX_RUNTIME_COMMON_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeLauncher_win.cpp
    )
    target_include_directories(slicer_linux_runtime PRIVATE ${SLICER_LINUX_RUNTIME_INCLUDE_DIRS})
    target_compile_definitions(slicer_linux_runtime PRIVATE SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS=1 BAMBU_EXPORTS)
    target_link_libraries(slicer_linux_runtime PRIVATE boost_headeronly boost_libs OpenSSL::Crypto)
elseif (APPLE)
    add_library(slicer_linux_runtime SHARED
        ${SLICER_LINUX_RUNTIME_COMMON_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeLauncher_mac.mm
    )
    target_include_directories(slicer_linux_runtime PRIVATE ${SLICER_LINUX_RUNTIME_INCLUDE_DIRS})
    target_compile_definitions(slicer_linux_runtime PRIVATE SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS=1)
    target_link_libraries(slicer_linux_runtime PRIVATE boost_headeronly boost_libs OpenSSL::Crypto)
else()
    add_library(slicer_linux_runtime SHARED
        ${SLICER_LINUX_RUNTIME_COMMON_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeLauncher_linux.cpp
    )
    target_include_directories(slicer_linux_runtime PRIVATE ${SLICER_LINUX_RUNTIME_INCLUDE_DIRS})
    target_link_libraries(slicer_linux_runtime PRIVATE boost_headeronly boost_libs OpenSSL::Crypto)
endif()

set_target_properties(slicer_linux_runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

install(TARGETS slicer_linux_runtime
    RUNTIME DESTINATION "."
    LIBRARY DESTINATION "."
)

if (UNIX AND NOT APPLE AND EXISTS "${SLICER_LINUX_RUNTIME_LINUX_HOST_DIR}/main.cpp" AND EXISTS "${SLICER_LINUX_RUNTIME_LINUX_HOST_DIR}/LinuxRuntimeHost.cpp")
    set(SLICER_LINUX_RUNTIME_HOST_COMMON_SOURCES
        ${SLICER_LINUX_RUNTIME_LINUX_HOST_DIR}/main.cpp
        ${SLICER_LINUX_RUNTIME_LINUX_HOST_DIR}/LinuxRuntimeHost.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeConfig.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcProtocol.cpp
    )

    find_package(nlohmann_json CONFIG QUIET)

    function(add_linux_runtime_host_variant target_name abi_value)
        add_executable(${target_name}
            ${SLICER_LINUX_RUNTIME_HOST_COMMON_SOURCES}
        )
        target_include_directories(${target_name} PRIVATE ${SLICER_LINUX_RUNTIME_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PRIVATE
            SLICER_LINUX_RUNTIME_STANDALONE_HOST=1
            SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS=1
            _GLIBCXX_USE_CXX11_ABI=${abi_value}
        )
        if (TARGET nlohmann_json::nlohmann_json)
            target_link_libraries(${target_name} PRIVATE nlohmann_json::nlohmann_json)
        endif()
        target_link_libraries(${target_name} PRIVATE boost_headeronly ${CMAKE_DL_LIBS} OpenSSL::Crypto)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN"
        )
    endfunction()

    add_linux_runtime_host_variant(slicer_linux_runtime_host_abi1 1)
    add_linux_runtime_host_variant(slicer_linux_runtime_host_abi0 0)

    add_custom_target(slicer_linux_runtime_host
        DEPENDS slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0
    )

    install(TARGETS slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0
        RUNTIME DESTINATION "."
    )
endif()

if (APPLE)
    set(SLICER_LINUX_RUNTIME_MAC_WRAPPER_SCRIPT_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper)
    set(SLICER_LINUX_RUNTIME_MAC_INSTALLER_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/macos/install_runtime_macos.sh)
    set(SLICER_LINUX_RUNTIME_MAC_VERIFY_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/macos/verify_runtime_macos.sh)
    set(SLICER_LINUX_RUNTIME_MAC_INSTANCE_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/macos/slicer_linux_runtime_lima_instance.txt)
    set(SLICER_LINUX_RUNTIME_MAC_RUNTIME_SOURCE_DIR ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime_host/runtime/linux-x86_64)
    set(SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES)

    set(SLICER_LINUX_RUNTIME_MAC_REQUIRED_RUNTIME_FILE_NAMES
        slicer_linux_runtime_host
        slicer_linux_runtime_host_abi1
        slicer_linux_runtime_host_abi0
        ld-linux-x86-64.so.2
        libc.so.6
        libm.so.6
        libresolv.so.2
        libnss_dns.so.2
        libnss_files.so.2
        libstdc++.so.6
        libgcc_s.so.1
        libz.so.1
        ca-certificates.crt
        slicer_base64.cer
    )

    foreach(SLICER_LINUX_RUNTIME_MAC_REQUIRED_RUNTIME_FILE_NAME ${SLICER_LINUX_RUNTIME_MAC_REQUIRED_RUNTIME_FILE_NAMES})
        if (NOT EXISTS "${SLICER_LINUX_RUNTIME_MAC_RUNTIME_SOURCE_DIR}/${SLICER_LINUX_RUNTIME_MAC_REQUIRED_RUNTIME_FILE_NAME}")
            message(FATAL_ERROR
                "Missing macOS Linux runtime file: ${SLICER_LINUX_RUNTIME_MAC_RUNTIME_SOURCE_DIR}/${SLICER_LINUX_RUNTIME_MAC_REQUIRED_RUNTIME_FILE_NAME}. "
                "Build/package linux-x86_64 runtime first with tools/slicer_linux_runtime_host/package_linux_host_runtime.sh on x86_64 Linux before configuring the macOS build."
            )
        endif()
    endforeach()

    file(GLOB SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES
        "${SLICER_LINUX_RUNTIME_MAC_RUNTIME_SOURCE_DIR}/*"
    )
    list(FILTER SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES EXCLUDE REGEX "/(libbambu_networking|libBambuSource|liblive555|libagora_rtc_sdk|libagora-fdkaac)[.]so([.].*)?$")
    list(FILTER SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES EXCLUDE REGEX "/(linux_component_manifest|network_plugins)[.]json$")

    add_custom_command(TARGET slicer_linux_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SLICER_LINUX_RUNTIME_MAC_WRAPPER_SCRIPT_SOURCE}"
            "$<TARGET_FILE_DIR:slicer_linux_runtime>/slicer-linux-runtime-host-wrapper"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SLICER_LINUX_RUNTIME_MAC_INSTALLER_SOURCE}"
            "$<TARGET_FILE_DIR:slicer_linux_runtime>/install_runtime_macos.sh"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SLICER_LINUX_RUNTIME_MAC_VERIFY_SOURCE}"
            "$<TARGET_FILE_DIR:slicer_linux_runtime>/verify_runtime_macos.sh"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SLICER_LINUX_RUNTIME_MAC_INSTANCE_SOURCE}"
            "$<TARGET_FILE_DIR:slicer_linux_runtime>/slicer_linux_runtime_lima_instance.txt"
    )

    foreach(SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILE ${SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES})
        get_filename_component(SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILE_NAME "${SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILE}" NAME)
        add_custom_command(TARGET slicer_linux_runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILE}"
                "$<TARGET_FILE_DIR:slicer_linux_runtime>/${SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILE_NAME}"
        )
    endforeach()

    install(PROGRAMS "${SLICER_LINUX_RUNTIME_MAC_WRAPPER_SCRIPT_SOURCE}"
        DESTINATION "."
        RENAME slicer-linux-runtime-host-wrapper
    )
    install(PROGRAMS "${SLICER_LINUX_RUNTIME_MAC_INSTALLER_SOURCE}"
        DESTINATION "."
        RENAME install_runtime_macos.sh
    )
    install(PROGRAMS "${SLICER_LINUX_RUNTIME_MAC_VERIFY_SOURCE}"
        DESTINATION "."
        RENAME verify_runtime_macos.sh
    )
    install(FILES "${SLICER_LINUX_RUNTIME_MAC_INSTANCE_SOURCE}"
        DESTINATION "."
        RENAME slicer_linux_runtime_lima_instance.txt
    )
    if (SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES)
        install(FILES ${SLICER_LINUX_RUNTIME_MAC_RUNTIME_FILES}
            DESTINATION "."
        )
    endif()
endif()

if (WIN32)
    set(SLICER_LINUX_RUNTIME_WSL_HELPER_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_wsl_run_host.sh)
    set(SLICER_LINUX_RUNTIME_WSL_INSTALLER_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/wsl/install_runtime.ps1)
    set(SLICER_LINUX_RUNTIME_WSL_INSTALLER_CMD_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/wsl/install_runtime.cmd)
    set(SLICER_LINUX_RUNTIME_WSL_VERIFY_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/wsl/verify_runtime.ps1)
    set(SLICER_LINUX_RUNTIME_WSL_DISTRO_FILE_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_wsl_distro.txt)
    set(SLICER_LINUX_RUNTIME_ASSEMBLER_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/release/assemble_windows_runtime_bundle.ps1)
    set(SLICER_LINUX_RUNTIME_COMPONENT_CACHE_SUBDIR_SOURCE ${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_component_dir.txt)
    add_custom_command(TARGET slicer_linux_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_WSL_HELPER_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/slicer_linux_runtime_wsl_run_host.sh
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_WSL_INSTALLER_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/install_runtime.ps1
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_WSL_INSTALLER_CMD_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/install_runtime.cmd
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_WSL_VERIFY_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/verify_runtime.ps1
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_WSL_DISTRO_FILE_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/slicer_linux_runtime_wsl_distro.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_ASSEMBLER_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/assemble_windows_runtime_bundle.ps1
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SLICER_LINUX_RUNTIME_COMPONENT_CACHE_SUBDIR_SOURCE}
            $<TARGET_FILE_DIR:slicer_linux_runtime>/slicer_linux_runtime_component_dir.txt
    )
    install(FILES
        ${SLICER_LINUX_RUNTIME_WSL_HELPER_SOURCE}
        ${SLICER_LINUX_RUNTIME_WSL_INSTALLER_SOURCE}
        ${SLICER_LINUX_RUNTIME_WSL_INSTALLER_CMD_SOURCE}
        ${SLICER_LINUX_RUNTIME_WSL_VERIFY_SOURCE}
        ${SLICER_LINUX_RUNTIME_WSL_DISTRO_FILE_SOURCE}
        ${SLICER_LINUX_RUNTIME_ASSEMBLER_SOURCE}
        ${SLICER_LINUX_RUNTIME_COMPONENT_CACHE_SUBDIR_SOURCE}
        DESTINATION "."
    )
endif()
