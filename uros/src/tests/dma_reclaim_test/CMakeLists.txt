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

# hal.defs, user half: the second half of #513 needs a task that BINDS a device
# and then dies, so this program registers as a driver, reports a probe, and is
# gone -- which is the only way the HAL's dead-name release has a witness.
set(DR_HAL_GENERATED)
add_mig_userland(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                 ${_DR_OUT} DR_HAL)
list(REMOVE_ITEM DR_HAL_GENERATED ${_DR_OUT}/hal_server.c)
list(APPEND DR_MIG_GENERATED ${DR_HAL_GENERATED})

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
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
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
  libnetname
  libcap
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

# ===========================================================================
# The manifest — what this program is allowed to hold (#513)
# ===========================================================================
#
# 🔴 IT DECLARES A PCI DEVICE, which cap_test's manifest deliberately does not.
# The two say opposite things on purpose: cap_test [13] reads back a REFUSAL for
# a class it never declared, and this one has to be ISSUED one, because the
# second half of #513 is a task that really claims a device and then dies.
#
# ⚠️ The class is the HOST BRIDGE and that is a safety decision, not a
# convenience.  This program takes a device and abandons it; picking the disk
# controller would mean that a release which did not work takes the boot disk
# with it, and a test that breaks the mature suite when it fails is a test
# nobody will keep.  0:0.0 is on every board these targets run on and no driver
# will ever want it.
set(DR_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/dma_reclaim_test.manifest)
set(DR_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/dma_reclaim_test.cmf")

add_custom_command(
  OUTPUT ${DR_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${DR_MANIFEST_OUT} ${DR_MANIFEST_SRC}
  DEPENDS mkmanifest ${DR_MANIFEST_SRC}
  COMMENT "mkmanifest: dma_reclaim_test.manifest -> dma_reclaim_test.cmf"
  VERBATIM
)

add_custom_target(dma_reclaim_test_manifest ALL DEPENDS ${DR_MANIFEST_OUT})
add_dependencies(dma_reclaim_test_server dma_reclaim_test_manifest)
