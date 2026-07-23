cmake_minimum_required(VERSION 3.16)
project(hello_world C)

# hello_world (#286) — the textbook Unix hello world: a musl-linked
# program that prints via printf and returns 0.  Proves that a standard
# program launched from ush gets its stdout wired to the controlling
# tty (libposix-uros binds fds 0/1/2 to the session ctty).  Same link
# recipe as fd_exec_test; static ET_EXEC so exec_server can load it.
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(hello_world_bin
  hello_world.c
)

add_dependencies(hello_world_bin mig_mach_public_headers musl_extproj)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

execute_process(
  COMMAND ${CMAKE_C_COMPILER} -m32 -print-file-name=include
  OUTPUT_VARIABLE _GCC_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND ${CMAKE_C_COMPILER} -m32 -print-libgcc-file-name
  OUTPUT_VARIABLE _LIBGCC_A
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_options(hello_world_bin PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -w
  -include stdint.h
  -nostdinc
)

target_include_directories(hello_world_bin PRIVATE
  ${MUSL_PREFIX}/include
  ${_GCC_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/lib/libposix-uros/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hello_world_bin PRIVATE
  -Wl,--start-group
  libcap
  libnetname
  libvfs
  libflipc2
  libmach_core
  libposix-uros
  ${MUSL_PREFIX}/lib/libc-musl.a
  ${_LIBGCC_A}
  -Wl,--end-group
  # #288: force-pull the real syscall dispatcher + TLS installer (shadowed
  # otherwise by musl's WEAK -ENOSYS stubs in a printf-only static binary).
  uros_musl_dispatcher
)

target_link_options(hello_world_bin PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(hello_world_bin PROPERTIES
  OUTPUT_NAME "hello_world"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
