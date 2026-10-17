cmake_minimum_required(VERSION 3.16)
project(boot_ports_test C)

# ===========================================================================
# Who bootstrap gives the master ports to (#620)
# ===========================================================================
#
# x86-64 only, which is the target this decides for; bootstrap is the same
# code on both, and i386's drivers ask it for the same ports through the same
# flags.  Started three times by bootstrap.conf -- none, -D, -H -D -- and the
# argument names the line.
#
# ⚠️ No MIG of its own: bootstrap_ports(), bootstrap_console(), device_open()
# and host_processor_sets() are all libmach's stubs already.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

add_executable(boot_ports_test_bin boot_ports_test.c)

target_compile_options(boot_ports_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(boot_ports_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(boot_ports_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/x86_64
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/include)

# --start-group for the same reason act_test needs it: libpthreads and libmach
# reach into each other.
target_link_libraries(boot_ports_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(boot_ports_test_bin libmach libpthreads
                 mig_mach_public_headers)

target_link_options(boot_ports_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(boot_ports_test_bin PROPERTIES
  OUTPUT_NAME "boot_ports_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
