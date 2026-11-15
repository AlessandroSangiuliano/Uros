cmake_minimum_required(VERSION 3.16)
project(fork_tty_test C)

# fork_tty_test (#948): a child made by fork() writes on the terminal it
# inherited.  musl_test's recipe, because fork() and /dev/tty are
# libposix-uros's.  It goes on the root disk and is typed at ush, the one place
# with a controlling terminal (scripts/ush-x86_64.py).
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(fork_tty_test_bin
  fork_tty_test.c
)

add_dependencies(fork_tty_test_bin mig_mach_public_headers musl_extproj)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

target_compile_options(fork_tty_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -Wall -Wextra
  -nostdinc
)

target_include_directories(fork_tty_test_bin BEFORE PRIVATE
  ${MUSL_PREFIX}/include
)

target_link_libraries(fork_tty_test_bin PRIVATE
  uros_libc
)

target_link_options(fork_tty_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(fork_tty_test_bin PROPERTIES
  OUTPUT_NAME "fork_tty_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
