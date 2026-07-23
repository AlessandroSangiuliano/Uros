cmake_minimum_required(VERSION 3.16)
project(fd_exec_test C)

# musl-linked target for the execve fd-inheritance smoke (#262 step 3).
# Same link recipe as hello_server (musl + libposix-uros + libvfs +
# libmach_core), but a tiny program: read an inherited fd and exit with
# a status the parent checks.  Static ET_EXEC so exec_server can load it.
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(fd_exec_test_bin
  fd_exec_test.c
)

add_dependencies(fd_exec_test_bin mig_mach_public_headers musl_extproj)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
  OUTPUT_VARIABLE _GCC_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-libgcc-file-name
  OUTPUT_VARIABLE _LIBGCC_A
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_options(fd_exec_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -w
  -include stdint.h
  -nostdinc
)

target_include_directories(fd_exec_test_bin PRIVATE
  ${MUSL_PREFIX}/include
  ${_GCC_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/lib/libposix-uros/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(fd_exec_test_bin PRIVATE
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
  # #288: force-pull the real syscall dispatcher + TLS installer, otherwise
  # musl's WEAK -ENOSYS stubs win the link for this minimal static binary.
  uros_musl_dispatcher
)

target_link_options(fd_exec_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(fd_exec_test_bin PROPERTIES
  OUTPUT_NAME "fd_exec_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
