cmake_minimum_required(VERSION 3.16)
project(mach_error_test C)

# mach_error_test (#917): one source, two programs of musl that call
# mach_error() and mach_error_string() -- /mach_error_test linked statically
# against uros_libc, as hello_world is, and /mach_error_dyn against the
# umbrella libc.so, as hello_dyn_world is.  Both go on the disk, and
# musl_test runs them by path as [27] and [28].

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR     ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin)

execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
  OUTPUT_VARIABLE _GCC_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# The Mach headers both builds read, after musl's.  The static build would
# have uapi from uros_libc's parts as well; the dynamic one is a command of
# its own, and has only what this list gives it.
set(MACH_ERROR_TEST_INCLUDES
  ${MUSL_PREFIX}/include
  ${_GCC_INCLUDE}
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_BINARY_DIR}/generated/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# ── Static, against uros_libc ───────────────────────────────────────────
add_executable(mach_error_test_bin mach_error_test.c)
add_dependencies(mach_error_test_bin mig_mach_public_headers musl_extproj)
target_compile_options(mach_error_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -Wall -Wextra
  -include stdint.h
  -nostdinc
)
target_include_directories(mach_error_test_bin PRIVATE ${MACH_ERROR_TEST_INCLUDES})
target_link_libraries(mach_error_test_bin PRIVATE uros_libc)
target_link_options(mach_error_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  # crt0 references __uros_libc_init weakly; see hello_world.
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)
set_target_properties(mach_error_test_bin PROPERTIES
  OUTPUT_NAME "mach_error_test"
  RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
)

# ── Dynamic, against the umbrella libc.so ───────────────────────────────
set(MACH_ERROR_DYN          ${OUT_DIR}/mach_error_dyn)
set(MACH_ERROR_DYN_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/mach_error_dyn.manifest)
set(MACH_ERROR_DYN_CMF      ${CMAKE_CURRENT_BINARY_DIR}/mach_error_dyn.cmf)
execute_process(
  COMMAND cc ${UROS_ARCH_FLAGS} -print-libgcc-file-name
  OUTPUT_VARIABLE MACH_ERROR_DYN_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(_dyn_inc "")
foreach(_d IN LISTS MACH_ERROR_TEST_INCLUDES)
  list(APPEND _dyn_inc -I${_d})
endforeach()

add_custom_command(
  OUTPUT ${MACH_ERROR_DYN}
  # As hello_dyn_world: a PIE whose PT_INTERP is the umbrella, which exports
  # mach_error and mach_error_string (contrib/uros_umbrella_mach.list).
  COMMAND cc ${UROS_ARCH_FLAGS} -nostdinc -fPIE -pie -fno-stack-protector
            -std=gnu11 -Wall -Wextra -include stdint.h
            ${_dyn_inc}
            -nostdlib
            ${MUSL_PREFIX}/lib/Scrt1.o
            ${MUSL_PREFIX}/lib/crti.o
            ${CMAKE_CURRENT_SOURCE_DIR}/mach_error_test.c
            -L${MUSL_PREFIX}/lib -lc
            ${MACH_ERROR_DYN_LIBGCC}
            ${MUSL_PREFIX}/lib/crtn.o
            -Wl,-dynamic-linker,/lib/ld-musl-${UROS_TARGET_ARCH}.so.1
            -Wl,--build-id=none
            -o ${MACH_ERROR_DYN}
  COMMAND $<TARGET_FILE:mkmanifest> -o ${MACH_ERROR_DYN_CMF} ${MACH_ERROR_DYN_MANIFEST}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${MACH_ERROR_DYN_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            ${MACH_ERROR_DYN}
  DEPENDS mach_error_test.c ${MACH_ERROR_DYN_MANIFEST}
          mkmanifest musl_umbrella mig_mach_public_headers
  COMMENT "Linking mach_error_dyn (dynamic PIE against umbrella libc.so, #917)"
  VERBATIM
)
add_custom_target(mach_error_dyn ALL DEPENDS ${MACH_ERROR_DYN})
