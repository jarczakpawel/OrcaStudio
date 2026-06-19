set(_srcdir ${CMAKE_CURRENT_LIST_DIR}/mpfr)

if (MSVC)
    set(_output  ${DESTDIR}/include/mpfr.h
                 ${DESTDIR}/include/mpf2mpfr.h
                 ${DESTDIR}/lib/libmpfr-4.lib
                 ${DESTDIR}/bin/libmpfr-4.dll)

    set(_mpfr_sources
        ${_srcdir}/include/mpfr.h
        ${_srcdir}/include/mpf2mpfr.h
        ${_srcdir}/lib/win-${DEPS_ARCH}/libmpfr-4.lib
        ${_srcdir}/lib/win-${DEPS_ARCH}/libmpfr-4.dll
    )

    add_custom_target(dep_MPFR
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DESTDIR}/include ${DESTDIR}/lib ${DESTDIR}/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/include/mpfr.h ${DESTDIR}/include/mpfr.h
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/include/mpf2mpfr.h ${DESTDIR}/include/mpf2mpfr.h
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/lib/win-${DEPS_ARCH}/libmpfr-4.lib ${DESTDIR}/lib/libmpfr-4.lib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcdir}/lib/win-${DEPS_ARCH}/libmpfr-4.dll ${DESTDIR}/bin/libmpfr-4.dll
        BYPRODUCTS ${_output}
        DEPENDS ${_mpfr_sources}
        VERBATIM
    )
    add_dependencies(dep_MPFR dep_GMP)

else ()


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

    ExternalProject_Add(dep_MPFR
        URL https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.bz2
            https://www.mpfr.org/mpfr-4.2.2/mpfr-4.2.2.tar.bz2
        URL_HASH SHA256=9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b
        DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/MPFR
        BUILD_IN_SOURCE ON
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${_gnu_m4_env} autoreconf -f -i
                          COMMAND ${CMAKE_COMMAND} -E env ${_gnu_m4_env} "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}" "CFLAGS=${_gmp_ccflags}" "CXXFLAGS=${_gmp_ccflags}" "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}" ./configure ${_cross_compile_arg} --prefix=${DESTDIR} --enable-shared=no --enable-static=yes --with-gmp=${DESTDIR} ${_gmp_build_tgt}
        BUILD_COMMAND make -j
        INSTALL_COMMAND make install
        DEPENDS dep_GMP
    )
endif ()
