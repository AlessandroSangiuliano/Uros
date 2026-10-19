cmake_minimum_required(VERSION 3.16)
project(taskpanic_load C)

# ===========================================================================
# The load -t needs (#705)
# ===========================================================================
#
# x86-64 only, where -t is (#688).  In both bundles, with -H, because it reads
# the kernel's command line with host_get_boot_info(); on a boot without -t it
# says so and ends.
#
# ⚠️ No MIG of its own: bootstrap_ports(), host_get_boot_info() and
# task_info() are all libmach's stubs already.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

add_executable(taskpanic_load_bin taskpanic_load.c)

target_compile_options(taskpanic_load_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(taskpanic_load_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(taskpanic_load_bin PRIVATE
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
target_link_libraries(taskpanic_load_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(taskpanic_load_bin libmach libpthreads
                 mig_mach_public_headers)

target_link_options(taskpanic_load_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(taskpanic_load_bin PROPERTIES
  OUTPUT_NAME "taskpanic_load"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
