cmake_minimum_required(VERSION 3.16)
project(block_device_server C)

# ===========================================================================
# MIG-generated device_master user stubs
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(DEVICE_MASTER_DEFS ${CMAKE_SOURCE_DIR}/src/mach_kernel/device/device_master.defs)

set(DM_USER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_user.c)
set(DM_USER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master.h)
set(DM_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_server.c)
set(DM_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master_server.h)

add_custom_command(
  OUTPUT ${DM_USER_C} ${DM_USER_H} ${DM_SERVER_C} ${DM_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DM_USER_H}
    -sheader ${DM_SERVER_H}
    -user ${DM_USER_C}
    -server ${DM_SERVER_C}
    ${DEVICE_MASTER_DEFS}
  DEPENDS migcom ${DEVICE_MASTER_DEFS}
  COMMENT "MIG: device_master.defs (user stubs for block_device_server)"
  VERBATIM
)

# ===========================================================================
# MIG-generated ahci_batch server stubs (batch device writes)
# ===========================================================================

set(AHCI_BATCH_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/ahci/ahci_batch.defs)

set(AB_USER_C ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_user.c)
set(AB_USER_H ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch.h)
set(AB_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_server.c)
set(AB_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_server.h)

add_custom_command(
  OUTPUT ${AB_USER_C} ${AB_USER_H} ${AB_SERVER_C} ${AB_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}/ahci
    -header ${AB_USER_H}
    -sheader ${AB_SERVER_H}
    -user ${AB_USER_C}
    -server ${AB_SERVER_C}
    ${AHCI_BATCH_DEFS}
  DEPENDS migcom ${AHCI_BATCH_DEFS} ${CMAKE_CURRENT_SOURCE_DIR}/ahci/ahci_batch_types.h
  COMMENT "MIG: ahci_batch.defs (batch write server stubs for block_device_server)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(BLK_SOURCES
  block_server.c
  block_partition.c
  block_device.c
  nostdlib_stubs.c
  ahci/ahci_module.c
  ahci/ahci_io.c
  virtio_blk/virtio_module.c
  ${DM_USER_C}
  ${AB_SERVER_C}
  ${CMAKE_BINARY_DIR}/generated/device_server.c
)

# Mark generated device_server.c so CMake doesn't complain at configure time.
set_source_files_properties(
  ${CMAKE_BINARY_DIR}/generated/device_server.c
  PROPERTIES GENERATED TRUE
)

# Copy device_server.h locally to avoid polluting include path.
set(LOCAL_DEVICE_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/device_server.h)
add_custom_command(
  OUTPUT ${LOCAL_DEVICE_SERVER_H}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/generated/device_server.h
    ${LOCAL_DEVICE_SERVER_H}
  DEPENDS ${CMAKE_BINARY_DIR}/generated/device_server.h
  COMMENT "Copy device_server.h for block_device_server"
)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(block_device_server
  ${BLK_SOURCES}
  ${LOCAL_DEVICE_SERVER_H}
)

target_compile_options(block_device_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(block_device_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(block_device_server PRIVATE
  # MIG-generated headers (device_master.h, device_server.h, ahci_batch_server.h)
  ${CMAKE_CURRENT_BINARY_DIR}
  # Module headers (ahci/, virtio_blk/)
  ${CMAKE_CURRENT_SOURCE_DIR}/ahci
  ${CMAKE_CURRENT_SOURCE_DIR}/virtio_blk
  # Framework header
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  # Pre-built export headers (mach/mach_interface.h, device/device_reply.h, etc.)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Kernel headers (mach/, device/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(block_device_server PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_mach
  libnetname
  -Wl,--end-group
)

target_link_options(block_device_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

add_dependencies(block_device_server mig_device_header)

set_target_properties(block_device_server PROPERTIES
  OUTPUT_NAME "block_device_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
