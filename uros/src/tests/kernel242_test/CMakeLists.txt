cmake_minimum_required(VERSION 3.16)
project(kernel242_test C)

# Userspace exerciser for the kernel paths refactored by issue #242
# (no-goto cleanup).  Calls every kernel entry point we touched at least
# once and asserts expected return values, so a regression in the
# refactor shows up as a clear FAIL line instead of a missed code path.

# Generate user-side MIG stubs for mach_i386.defs (i386_set_ldt etc).
# These are not built anywhere else in the tree (the .defs file only
# produces the in-kernel server stubs), so we generate them locally.
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(MACH_I386_DEFS ${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/i386/mach_i386.defs)
set(MACH_I386_USER_C ${CMAKE_CURRENT_BINARY_DIR}/mach_i386_user.c)
set(MACH_I386_USER_H ${CMAKE_CURRENT_BINARY_DIR}/mach_i386.h)

add_custom_command(
  OUTPUT ${MACH_I386_USER_C} ${MACH_I386_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/i386
    -header ${MACH_I386_USER_H}
    -sheader /dev/null
    -user ${MACH_I386_USER_C}
    -server /dev/null
    ${MACH_I386_DEFS}
  DEPENDS migcom ${MACH_I386_DEFS}
  COMMENT "MIG: mach_i386.defs (user stubs for kernel242_test)"
  VERBATIM
)

add_executable(kernel242_test_server
  kernel242_test.c
  ${MACH_I386_USER_C}
)

add_dependencies(kernel242_test_server mig_mach_public_headers)

target_compile_options(kernel242_test_server PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(kernel242_test_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(kernel242_test_server PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(kernel242_test_server PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libmach
  -Wl,--end-group
)

target_link_options(kernel242_test_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(kernel242_test_server PROPERTIES
  OUTPUT_NAME "kernel242_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
