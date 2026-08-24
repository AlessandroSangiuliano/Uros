cmake_minimum_required(VERSION 3.16)
project(flipc_bench C)

# flipc_bench (#272) — standalone libvfs FLIPC v2 A/B benchmark, launched
# from the shell after boot (quiescent system) instead of running during
# the disk suite while every server contends for the CPU.  Reports the
# median of N runs per chunk so the FLIPC-vs-Mach ratio is stable.
# Same static-musl link recipe as hello_world; links libvfs + libflipc2.
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(flipc_bench_bin
  flipc_bench.c
)

add_dependencies(flipc_bench_bin mig_mach_public_headers musl_extproj libposix-uros)

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

target_compile_options(flipc_bench_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  
  -include stdint.h
  -nostdinc
)

target_include_directories(flipc_bench_bin PRIVATE
  ${MUSL_PREFIX}/include
  ${_GCC_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/lib/libposix-uros/include
  ${CMAKE_SOURCE_DIR}/src/lib/libvfs   # libvfs.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_BINARY_DIR}/generated/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(flipc_bench_bin PRIVATE
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

# #291: no -Wl,-u — the weak musl syscall/clone stubs were stripped from
# the static libc-musl.a, so the strong libposix-uros dispatcher is pulled
# automatically.
target_link_options(flipc_bench_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(flipc_bench_bin PROPERTIES
  OUTPUT_NAME "flipc_bench"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
