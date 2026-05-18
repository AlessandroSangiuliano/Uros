# libposix-uros — POSIX personality shim sitting under musl libc (#250).
#
# Only built when -DUROS_BUILD_MUSL=ON.  Produces a static archive that
# the smoke test (and, from Phase 3 onward, every migrated server)
# links alongside libc-musl.a.
#
# The library is self-contained: it talks to the kernel only through
# the five Mach trap trampolines in i386/traps.S (mach_print,
# mach_task_self, vm_allocate, vm_deallocate, task_terminate).  No
# dependency on libmach, MIG, or the rest of mach_services — that
# isolation is what lets Phase 2 ship without disturbing any existing
# server build.

if(NOT UROS_BUILD_MUSL)
  return()
endif()

cmake_minimum_required(VERSION 3.16)
project(libposix-uros C ASM)

add_library(libposix-uros STATIC
  syscall.c
  handlers.c
  i386/traps.S
)

target_compile_options(libposix-uros PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -m32 -march=i686 -fno-builtin
                          -ffreestanding -fno-stack-protector
                          -Wall -Wextra -Werror>
  $<$<COMPILE_LANGUAGE:ASM>:-m32>
)

# musl headers (errno.h, stddef.h, stdint.h) — published by the
# Phase 1 ExternalProject_Add under build/src/contrib/musl-install/.
# Using musl's own headers here keeps the shim ABI-coherent with the
# libc it lives under.
target_include_directories(libposix-uros PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/src/contrib/musl-install/include
)

# Ensure musl has been built before we try to compile against its
# headers.  ExternalProject targets aren't created until generation
# time, hence the conditional guard.
if(TARGET musl_extproj)
  add_dependencies(libposix-uros musl_extproj)
endif()

set_target_properties(libposix-uros PROPERTIES
  OUTPUT_NAME "posix-uros"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
