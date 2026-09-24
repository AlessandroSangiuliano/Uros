cmake_minimum_required(VERSION 3.16)
project(io_claim_race C)

# ===========================================================================
# Two tasks race one I/O range, on purpose (#538, done-when 7)
# ===========================================================================
#
# x86-64 only: the tables it races are the same on both targets, but the
# preemption that makes the race reachable on one processor is x86-64's, and
# the harness that asks it with -smp is run-x86_64.sh.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

# device_master.defs, user half: the two RPCs it races are in the tail of
# that file.  add_mig_userland_client for cap_test's reason -- the server half
# exists in the kernel and a second copy would be a second demux from one
# .defs with nothing comparing them.
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

set(_ICR_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(ICR_MIG_GENERATED)
add_mig_userland_client(${UROS_UAPI_DIR}/device/device_master.defs
                        ${_ICR_OUT} ICR_MIG)

# hal.defs, user half, for arm [4] (#577): the window it asks about is found
# in the HAL's registry, because this program may not read configuration
# space.  _client for hal_bar_test's reason -- no second hal_server.h (#565).
set(ICR_HAL_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                        ${_ICR_OUT} ICR_HAL)
list(APPEND ICR_MIG_GENERATED ${ICR_HAL_GENERATED})

add_executable(io_claim_race_bin io_claim_race.c ${ICR_MIG_GENERATED})

add_dependencies(io_claim_race_bin libmach libpthreads libnetname
                 mig_mach_public_headers)

target_compile_options(io_claim_race_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h)

target_compile_definitions(io_claim_race_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(io_claim_race_bin PRIVATE
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${_ICR_OUT}
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(io_claim_race_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libmach
  -Wl,--end-group)

target_link_options(io_claim_race_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(io_claim_race_bin PROPERTIES
  OUTPUT_NAME "io_claim_race"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")
