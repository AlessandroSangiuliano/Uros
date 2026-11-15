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
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
    -I${CMAKE_SOURCE_DIR}/src/servers/exec_server
    -I${CMAKE_SOURCE_DIR}/src/servers/proc_server)

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

# service.defs, user half (#744): [34] asks bootstrap for a registered port's
# receive right that is not this task's to take, on both ports it could ask on.
set(CT_SVC_GENERATED)
add_mig_userland_client(${UROS_UAPI_DIR}/servers/service.defs ${_CT_OUT} CT_SVC)
list(APPEND CT_MIG_GENERATED ${CT_SVC_GENERATED})

# exec.defs, user half (#743): [42] execs a program with its own per-task
# port, as libposix-uros does, to see the program's port derived from it.
set(CT_EXEC_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/exec_server/exec.defs
                        ${_CT_OUT} CT_EXEC)
list(APPEND CT_MIG_GENERATED ${CT_EXEC_GENERATED})

# proc.defs, user half (#927): [45] registers a child with proc_server by its
# reduced port, and by its full task port, which must be refused.
set(CT_PROC_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/proc_server/proc.defs
                        ${_CT_OUT} CT_PROC)
list(APPEND CT_MIG_GENERATED ${CT_PROC_GENERATED})

add_executable(cap_test_server
  cap_test.c ${CT_MIG_GENERATED}
)

add_dependencies(cap_test_server mig_mach_public_headers libcap)

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
  # #743: exec_types.h, which the exec.h MIG generates for [42] imports.
  ${CMAKE_SOURCE_DIR}/src/servers/exec_server
  # #927: proc_types.h, which proc.h imports, for [45].
  ${CMAKE_SOURCE_DIR}/src/servers/proc_server
  # #743: cap_server.h, the MIG stubs libcap links in, for the arms that ask
  # a port of their own choosing.  Last, so that it shadows nothing.
  ${CMAKE_BINARY_DIR}/generated
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

# ===========================================================================
# The manifest (#432) — the first policy this test is subject to.
# ===========================================================================
#
# 🔑 Compiled HERE and consumed in two different ways, because the two targets
# assemble their bundle with two different tools.  x86-64's bundle is built by
# CMake in servers/bootstrap and names the .cmf directly; i386's is built by
# scripts/make-bundle.sh, which picks manifests out of the export tree.  This
# rule writes to the export tree, and the bootstrap rule reads from it -- so
# the file is produced once and both bundles ship the same bytes.
set(CAPTEST_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/cap_test.manifest)
set(CAPTEST_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/cap_test.cmf")

add_custom_command(
  OUTPUT ${CAPTEST_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${CAPTEST_MANIFEST_OUT}
          ${CAPTEST_MANIFEST_SRC}
  DEPENDS mkmanifest ${CAPTEST_MANIFEST_SRC}
  COMMENT "mkmanifest: cap_test.manifest -> cap_test.cmf"
  VERBATIM
)

add_custom_target(cap_test_manifest ALL DEPENDS ${CAPTEST_MANIFEST_OUT})
add_dependencies(cap_test_server cap_test_manifest)
