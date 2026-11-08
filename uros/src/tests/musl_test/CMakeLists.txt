cmake_minimum_required(VERSION 3.16)
project(musl_test C)

# musl_test (#414): a static C program, POSIX and nothing else, whose arms each
# read back what they did.  hello_world's recipe, because that is what a program
# on this system is linked with.
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(musl_test_bin
  musl_test.c
)

add_dependencies(musl_test_bin mig_mach_public_headers musl_extproj)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-libgcc-file-name
  OUTPUT_VARIABLE _LIBGCC_A
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_options(musl_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -Wall -Wextra
  -nostdinc
)

target_include_directories(musl_test_bin BEFORE PRIVATE
  ${MUSL_PREFIX}/include
)

target_link_libraries(musl_test_bin PRIVATE
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

target_link_options(musl_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(musl_test_bin PROPERTIES
  OUTPUT_NAME "musl_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
