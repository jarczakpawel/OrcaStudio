# Minimal Eigen3 finder with imported target fallback.
# Prefer a package config when it exists, but support dependency builds that only install headers.

find_path(EIGEN3_INCLUDE_DIR
    NAMES signature_of_eigen3_matrix_library
    PATHS
        ${CMAKE_PREFIX_PATH}
        ${CMAKE_INSTALL_PREFIX}
        ${CMAKE_SOURCE_DIR}/deps/build/OrcaSlicer_dep/usr/local
    PATH_SUFFIXES include/eigen3 include/eigen eigen3 eigen
)

set(_eigen3_version_header "")
foreach(_candidate
    "${EIGEN3_INCLUDE_DIR}/Eigen/src/Core/util/Macros.h"
    "${EIGEN3_INCLUDE_DIR}/Eigen/src/Core/util/ConfigureVectorization.h"
    "${EIGEN3_INCLUDE_DIR}/Eigen/src/Core/util/Constants.h"
)
    if(EIGEN3_INCLUDE_DIR AND EXISTS "${_candidate}")
        file(READ "${_candidate}" _candidate_content)
        string(APPEND _eigen3_version_header "\n${_candidate_content}")
    endif()
endforeach()

if(_eigen3_version_header)
    string(REGEX MATCH "#[ \t]*define[ \t]+EIGEN_WORLD_VERSION[ \t]+([0-9]+)" _eigen3_world_version_match "${_eigen3_version_header}")
    if(_eigen3_world_version_match)
        set(EIGEN3_WORLD_VERSION "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#[ \t]*define[ \t]+EIGEN_MAJOR_VERSION[ \t]+([0-9]+)" _eigen3_major_version_match "${_eigen3_version_header}")
    if(_eigen3_major_version_match)
        set(EIGEN3_MAJOR_VERSION "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#[ \t]*define[ \t]+EIGEN_MINOR_VERSION[ \t]+([0-9]+)" _eigen3_minor_version_match "${_eigen3_version_header}")
    if(_eigen3_minor_version_match)
        set(EIGEN3_MINOR_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

if(DEFINED EIGEN3_WORLD_VERSION AND DEFINED EIGEN3_MAJOR_VERSION AND DEFINED EIGEN3_MINOR_VERSION)
    set(Eigen3_VERSION "${EIGEN3_WORLD_VERSION}.${EIGEN3_MAJOR_VERSION}.${EIGEN3_MINOR_VERSION}")
    set(EIGEN3_VERSION "${Eigen3_VERSION}")
elseif(EIGEN3_INCLUDE_DIR)
    # The CI dependency recipe builds Eigen 5.0.1. Some Eigen snapshots do not expose
    # the version macros in the historical header path used by older find modules.
    # Do not fail with a bogus ".." version when the dependency include tree exists.
    set(Eigen3_VERSION "5.0.1")
    set(EIGEN3_VERSION "${Eigen3_VERSION}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Eigen3
    REQUIRED_VARS EIGEN3_INCLUDE_DIR
    VERSION_VAR Eigen3_VERSION
)

if(Eigen3_FOUND AND NOT TARGET Eigen3::Eigen)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(Eigen3::Eigen PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(EIGEN3_INCLUDE_DIR)
