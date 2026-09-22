cmake_minimum_required(VERSION 3.16)
project(hal_bar_test C)

# What the HAL says about a device's regions, and whether one above four
# gigabytes can be reached (#427).
#
# Two MIG clients and no server half: this program asks the HAL for the
# registry and asks the kernel to map a physical range, and answers nothing.
#
# ⚠️ add_mig_userland and NOT scripts/mig, for the reason cap_test's file
# gives: that wrapper routes -target to the C preprocessor, and migcom then
# silently defaults to i386 (#427).
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(_HB_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(HB_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_HB_OUT} HB_MIG)
list(REMOVE_ITEM HB_MIG_GENERATED ${_HB_OUT}/device_master_server.c)

set(HB_HAL_GENERATED)
# ⚠️ _client and not add_mig_userland: this program CALLS the HAL and answers
# none of it, so the server half is not generated rather than generated and
# then removed from the sources again.  What is not generated cannot shadow
# the HAL's own hal_server.h (#565).
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                        ${_HB_OUT} HB_HAL)
list(APPEND HB_MIG_GENERATED ${HB_HAL_GENERATED})

add_executable(hal_bar_test_server
  hal_bar_test.c ${HB_MIG_GENERATED}
)

add_dependencies(hal_bar_test_server mig_mach_public_headers)

target_compile_options(hal_bar_test_server PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(hal_bar_test_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(hal_bar_test_server PRIVATE
  # The HAL's own directory comes first, and it no longer has to (#565).
  #
  # 🔴 IT USED TO BE LOAD-BEARING, and this comment used to say so: migcom
  # wrote a file called hal_server.h -- the SERVER-side header of hal.defs --
  # into the generated directory below, and hal_server.h is also the name of
  # the HAL's own header, the one that defines struct hal_device_info.  With
  # the generated directory first, `#include "hal_server.h"' found a header
  # full of MIG declarations and the record this program is written about was
  # an incomplete type.
  #
  # 🔑 A build whose correctness rests on the order of two -I flags is one edit
  # away from being wrong and reports the choice to nobody.  The MIG call above
  # asks for the user half only now, so there is no second hal_server.h to
  # order against -- this program is a client of the HAL and answers no RPC,
  # which was already why the server .c was being removed again by hand.
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${_HB_OUT}
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hal_bar_test_server PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libmach
  -Wl,--end-group
)

target_link_options(hal_bar_test_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(hal_bar_test_server PROPERTIES
  OUTPUT_NAME "hal_bar_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
