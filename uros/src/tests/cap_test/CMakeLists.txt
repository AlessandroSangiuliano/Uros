cmake_minimum_required(VERSION 3.16)
project(cap_test C)

# device_master.defs, user half: #432 gave the device master an access check
# of its own -- a device has ONE driver -- and proving a REFUSAL needs a task
# that is not the driver.  This one holds the master device port and starts
# after the block server has claimed the disks, which is what makes it the
# place the check can be seen to fire.
#
# ⚠️ add_mig_userland and NOT scripts/mig: that wrapper routes a flag it does
# not know to the C preprocessor, so `-target x86_64' would reach cc instead of
# migcom, which then defaults to i386 and emits stubs with the wrong message
# layout, silently (#427).
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

set(_CT_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(CT_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_CT_OUT} CT_MIG)
list(REMOVE_ITEM CT_MIG_GENERATED ${_CT_OUT}/device_master_server.c)

# ahci_batch.defs, user half: device_read_phys is how a client asks a disk to
# DMA into pages the CLIENT owns, and #432 made that the interesting case --
# the pages belong to a task with no device, and the disk is confined.
set(CT_BATCH_GENERATED)
add_mig_userland(${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci/ahci_batch.defs
                 ${_CT_OUT} CT_BATCH)
list(REMOVE_ITEM CT_BATCH_GENERATED ${_CT_OUT}/ahci_batch_server.c)
list(APPEND CT_MIG_GENERATED ${CT_BATCH_GENERATED})

add_executable(cap_test_server
  cap_test.c ${CT_MIG_GENERATED}
)

add_dependencies(cap_test_server mig_mach_public_headers)

target_compile_options(cap_test_server PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  
  -include stdint.h
)

target_compile_definitions(cap_test_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(cap_test_server PRIVATE
  ${_CT_OUT}
  ${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
)

target_link_libraries(cap_test_server PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libcap
  libmach
  -Wl,--end-group
)

target_link_options(cap_test_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(cap_test_server PROPERTIES
  OUTPUT_NAME "cap_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
