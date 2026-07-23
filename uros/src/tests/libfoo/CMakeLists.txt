cmake_minimum_required(VERSION 3.16)
project(libfoo C)

# libfoo.so — toy shared object loaded at runtime by dlopen_test
# (#234 Phase 7 increment 4: dlopen end-to-end validation).
#
# Built as a stand-alone PIC .so via a custom command instead of
# add_library(SHARED) because the host CMake target compiler is the
# Linux toolchain — we want a freestanding -m32 .so with no host
# runtime (no GNU IFUNC, no host crt) so it can be loaded by ld-musl
# on Uros without dragging in anything the umbrella libc.so doesn't
# already provide.

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR     ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib)
set(LIBFOO_SO   ${OUT_DIR}/libfoo.so)

add_custom_command(
  OUTPUT ${LIBFOO_SO}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}
  COMMAND cc -m32 -nostdinc -fPIC -shared -fno-stack-protector
            -I${MUSL_PREFIX}/include
            -nostdlib
            ${CMAKE_CURRENT_SOURCE_DIR}/libfoo.c
            -Wl,-soname,libfoo.so
            -Wl,--build-id=none
            -o ${LIBFOO_SO}
  DEPENDS libfoo.c musl_umbrella
  COMMENT "Linking libfoo.so (toy shared object for dlopen test)"
  VERBATIM
)

add_custom_target(libfoo ALL DEPENDS ${LIBFOO_SO})
