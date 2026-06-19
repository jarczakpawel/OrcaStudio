cmake_minimum_required(VERSION 3.16)
project(slicer_linux_runtime_host_standalone LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG QUIET)

set(HOST_SOURCES
    "${REPO_ROOT}/tools/slicer_linux_runtime_host/main.cpp"
    "${REPO_ROOT}/tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp"
    "${REPO_ROOT}/src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.cpp"
    "${REPO_ROOT}/src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcProtocol.cpp"
)

function(add_host_variant target_name abi_value)
    add_executable(${target_name} ${HOST_SOURCES})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/slicer_linux_runtime_host"
        OUTPUT_NAME ${target_name}
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
    target_include_directories(${target_name} PRIVATE
        "${REPO_ROOT}"
        "${REPO_ROOT}/src"
        "${REPO_ROOT}/deps_src"
    )
    target_compile_definitions(${target_name} PRIVATE
        SLICER_LINUX_RUNTIME_STANDALONE_HOST=1
        SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS=1
        _GLIBCXX_USE_CXX11_ABI=${abi_value}
    )
    if (TARGET nlohmann_json::nlohmann_json)
        target_link_libraries(${target_name} PRIVATE nlohmann_json::nlohmann_json)
    endif()
    target_link_libraries(${target_name} PRIVATE
        Threads::Threads
        OpenSSL::Crypto
        ${CMAKE_DL_LIBS}
    )
endfunction()

add_host_variant(slicer_linux_runtime_host_abi1 1)
add_host_variant(slicer_linux_runtime_host_abi0 0)
add_custom_target(slicer_linux_runtime_host DEPENDS slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0)
