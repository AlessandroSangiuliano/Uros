cmake_minimum_required(VERSION 3.16)
project(static_lc_test C)

# static_lc_test (#929): a program linked as a build that is not ours links
# it -- the compiler with the musl-gcc.specs of the install directory,
# -static, and the -lc the driver adds -- so that it starts in musl's crt1 and
# takes libc.a from the install directory, which #929 made the static
# umbrella.  On the disk; musl_test runs it as [29].  x86-64 only: the specs
# carry no -m32, so on i386 the host compiler would build for the host.

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR     ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin)
set(STATIC_LC_TEST ${OUT_DIR}/static_lc_test)

add_custom_command(
  OUTPUT ${STATIC_LC_TEST}
  # Nothing of ours on this line but the specs' path: a foreign build has
  # that and nothing more.
  COMMAND ${CMAKE_C_COMPILER} -specs=${MUSL_PREFIX}/lib/musl-gcc.specs -static
            -O2 -Wall -Wextra -Wl,--build-id=none
            -o ${STATIC_LC_TEST}
            ${CMAKE_CURRENT_SOURCE_DIR}/static_lc_test.c
  # The static umbrella as a file: a program linked against it is linked
  # again when it is made again (#930).
  DEPENDS static_lc_test.c ${MUSL_PREFIX}/lib/libc.a uros_libc_archive musl_extproj
  COMMENT "Linking static_lc_test (-static with a plain -lc, through musl-gcc.specs, #929)"
  VERBATIM
)
add_custom_target(static_lc_test ALL DEPENDS ${STATIC_LC_TEST})
