cmake_minimum_required(VERSION 3.16)
project(dma_reclaim_test C)

# device_master.defs, user half.  This test needs only the DMA half of it and
# no capability at all: device_dma_alloc with DEVICE_DMA_NO_BDF is the path
# that names no device, so check_claim() lets it through.  That is what makes
# the program runnable before cap_server is up, and on a machine with no
# remapping hardware -- the reclaim it measures is the kernel's bookkeeping,
# not the IOMMU's.
#
# ⚠️ add_mig_userland and NOT scripts/mig, for the reason cap_test's file
# gives: that wrapper routes -target to the C preprocessor, and migcom then
# silently defaults to i386 (#427).
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(_DR_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(DR_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_DR_OUT} DR_MIG)
list(REMOVE_ITEM DR_MIG_GENERATED ${_DR_OUT}/device_master_server.c)

add_executable(dma_reclaim_test_server
  dma_reclaim_test.c ${DR_MIG_GENERATED}
)

add_dependencies(dma_reclaim_test_server mig_mach_public_headers)

target_compile_options(dma_reclaim_test_server PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(dma_reclaim_test_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(dma_reclaim_test_server PRIVATE
  ${_DR_OUT}
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(dma_reclaim_test_server PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group
)

target_link_options(dma_reclaim_test_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(dma_reclaim_test_server PROPERTIES
  OUTPUT_NAME "dma_reclaim_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
