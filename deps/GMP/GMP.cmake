
set(_srcdir ${CMAKE_CURRENT_LIST_DIR}/gmp)

if (IN_GIT_REPO)
    set(GMP_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_GMP-prefix/src/dep_GMP)
endif ()

if (MSVC)
    set(_output  ${DESTDIR}/include/gmp.h
                 ${DESTDIR}/lib/libgmp-10.lib
                 ${DESTDIR}/bin/libgmp-10.dll)

    set(_gmp_sources
        ${_srcdir}/include/gmp.h
        ${_srcdir}/lib/win-${DEPS_ARCH}/libgmp-10.lib
        ${_srcdir}/lib/win-${DEPS_ARCH}/libgmp-10.dll
    )

    add_custom_target(dep_GMP
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DESTDIR}/include ${DESTDIR}/lib ${DESTDIR}/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/include/gmp.h ${DESTDIR}/include/gmp.h
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/lib/win-${DEPS_ARCH}/libgmp-10.lib ${DESTDIR}/lib/libgmp-10.lib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/lib/win-${DEPS_ARCH}/libgmp-10.dll ${DESTDIR}/bin/libgmp-10.dll
        BYPRODUCTS ${_output}
        DEPENDS ${_gmp_sources}
        VERBATIM
    )

else ()
    set(_gmp_ccflags "-O2 -DNDEBUG -fPIC -DPIC -Wall -Wmissing-prototypes -Wpointer-arith -pedantic -fomit-frame-pointer -fno-common")
    set(_gmp_build_tgt "${CMAKE_SYSTEM_PROCESSOR}")

    if (APPLE)
        if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
            set(_gmp_build_arch aarch64)
        else ()
            set(_gmp_build_arch ${CMAKE_SYSTEM_PROCESSOR})
        endif()
        if (IS_CROSS_COMPILE)
            if (${CMAKE_OSX_ARCHITECTURES} MATCHES "arm")
                set(_gmp_host_arch aarch64)
                set(_gmp_host_arch_flags "-arch arm64")
            elseif (${CMAKE_OSX_ARCHITECTURES} MATCHES "x86_64")
                set(_gmp_host_arch x86_64)
                set(_gmp_host_arch_flags "-arch x86_64")
            endif()
            set(_gmp_ccflags "${_gmp_ccflags} ${_gmp_host_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}")
            set(_gmp_build_tgt --build=${_gmp_build_arch}-apple-darwin --host=${_gmp_host_arch}-apple-darwin)
        else ()
            set(_gmp_ccflags "${_gmp_ccflags} -mmacosx-version-min=${DEP_OSX_TARGET}")
            set(_gmp_build_tgt "--build=${_gmp_build_arch}-apple-darwin")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
            set(_gmp_ccflags "${_gmp_ccflags} -march=armv7-a") # Works on RPi-4
            set(_gmp_build_tgt armv7)
        endif()
        set(_gmp_build_tgt "--build=${_gmp_build_tgt}-pc-linux-gnu")
    else ()
        set(_gmp_build_tgt "") # let it guess
    endif()


    set(_gnu_m4_env)
    if (APPLE)
        function(_orcaslicer_find_gnu_m4 _out_exe _out_dir)
            set(_m4_candidates)
            if (DEFINED ENV{M4} AND NOT "$ENV{M4}" STREQUAL "")
                list(APPEND _m4_candidates "$ENV{M4}")
            endif ()
            list(APPEND _m4_candidates
                /opt/homebrew/opt/m4/bin/m4
                /opt/homebrew/opt/m4/bin/gm4
                /usr/local/opt/m4/bin/m4
                /usr/local/opt/m4/bin/gm4
                /opt/homebrew/bin/m4
                /opt/homebrew/bin/gm4
                /usr/local/bin/m4
                /usr/local/bin/gm4
            )
            foreach (_m4_candidate IN LISTS _m4_candidates)
                if (EXISTS "${_m4_candidate}")
                    execute_process(
                        COMMAND "${_m4_candidate}" --gnu --version
                        RESULT_VARIABLE _m4_result
                        OUTPUT_QUIET
                        ERROR_QUIET
                    )
                    if (_m4_result EQUAL 0)
                        get_filename_component(_m4_dir "${_m4_candidate}" DIRECTORY)
                        set(${_out_exe} "${_m4_candidate}" PARENT_SCOPE)
                        set(${_out_dir} "${_m4_dir}" PARENT_SCOPE)
                        return()
                    endif ()
                endif ()
            endforeach ()
            message(FATAL_ERROR "GNU m4 not found. Install Homebrew m4 and export M4 to its executable path.")
        endfunction()

        _orcaslicer_find_gnu_m4(_GNU_M4_EXECUTABLE _GNU_M4_DIR)
        set(_gnu_m4_env "M4=${_GNU_M4_EXECUTABLE}" "PATH=${_GNU_M4_DIR}:$ENV{PATH}")
    endif ()

    set(_cross_compile_arg "")
    if (CMAKE_CROSSCOMPILING)
        # TOOLCHAIN_PREFIX should be defined in the toolchain file
        set(_cross_compile_arg --host=${TOOLCHAIN_PREFIX})
    endif ()

    ExternalProject_Add(dep_GMP
        URL https://github.com/SoftFever/OrcaSlicer_deps/releases/download/gmp-6.2.1/gmp-6.2.1.tar.bz2
        URL_HASH SHA256=eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c
        DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/GMP
        PATCH_COMMAND git apply ${GMP_DIRECTORY_FLAG} --verbose ${CMAKE_CURRENT_LIST_DIR}/0001-GMP_GCC15.patch
        BUILD_IN_SOURCE ON
        CONFIGURE_COMMAND  ${CMAKE_COMMAND} -E env ${_gnu_m4_env} "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}" "CFLAGS=${_gmp_ccflags}" "CXXFLAGS=${_gmp_ccflags}" "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}" ./configure ${_cross_compile_arg} --enable-shared=no --enable-cxx=yes --enable-static=yes "--prefix=${DESTDIR}" ${_gmp_build_tgt}
        BUILD_COMMAND     make -j
        INSTALL_COMMAND   make install
    )
endif ()
