cmake_minimum_required(VERSION 3.16)
project(pthread_min C)

# pthread_min (#291) — minimal musl pthread regression: create/join workers
# + mutex contention.  Same static musl link recipe as hello_world, but
# crucially WITHOUT any -Wl,-u,__uros_clone workaround: it proves that
# stripping the weak uros_syscall_stub.lo from libc-musl.a (#291) is enough
# for pthread_create to pull the strong __uros_clone and succeed.
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(pthread_min_bin
  pthread_min.c
)

add_dependencies(pthread_min_bin mig_mach_public_headers musl_extproj)

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

target_compile_options(pthread_min_bin PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -w
  -include stdint.h
  -nostdinc
)

target_include_directories(pthread_min_bin PRIVATE
  ${MUSL_PREFIX}/include
  ${_GCC_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/lib/libposix-uros/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(pthread_min_bin PRIVATE
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
)

target_link_options(pthread_min_bin PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(pthread_min_bin PROPERTIES
  OUTPUT_NAME "pthread_min"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
