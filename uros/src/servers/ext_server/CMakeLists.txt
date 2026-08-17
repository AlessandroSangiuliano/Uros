cmake_minimum_required(VERSION 3.16)
project(ext_server C)

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)

# ===========================================================================
# MIG-generated ext2fs_server server stubs (subsystem 2920)
# ===========================================================================

set(EXT2_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/ext2fs_server.defs)

set(EXT2_USER_C ${CMAKE_CURRENT_BINARY_DIR}/ext2fs_server_user.c)
set(EXT2_USER_H ${CMAKE_CURRENT_BINARY_DIR}/ext2fs_server.h)
set(EXT2_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/ext2fs_server_server.c)
set(EXT2_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/ext2fs_server_server.h)

add_custom_command(
  OUTPUT ${EXT2_USER_C} ${EXT2_USER_H} ${EXT2_SERVER_C} ${EXT2_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${EXT2_USER_H}
    -sheader ${EXT2_SERVER_H}
    -user ${EXT2_USER_C}
    -server ${EXT2_SERVER_C}
    ${EXT2_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${EXT2_DEFS}
  COMMENT "MIG: ext2fs_server.defs (server stubs for ext2_server)"
  VERBATIM
)

# ===========================================================================
# MIG-generated vfs server stubs (subsystem 3000, #220 sub-step B)
# ===========================================================================

set(VFS_DEFS ${CMAKE_SOURCE_DIR}/src/lib/libvfs/vfs.defs)
set(VFS_TYPES_H ${CMAKE_SOURCE_DIR}/src/lib/libvfs/vfs_types.h)

set(VFS_USER_C ${CMAKE_CURRENT_BINARY_DIR}/vfs_user.c)
set(VFS_USER_H ${CMAKE_CURRENT_BINARY_DIR}/vfs.h)
set(VFS_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.c)
set(VFS_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.h)

add_custom_command(
  OUTPUT ${VFS_USER_C} ${VFS_USER_H} ${VFS_SERVER_C} ${VFS_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/lib/libvfs
    -header ${VFS_USER_H}
    -sheader ${VFS_SERVER_H}
    -user ${VFS_USER_C}
    -server ${VFS_SERVER_C}
    ${VFS_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${VFS_DEFS} ${VFS_TYPES_H}
  COMMENT "MIG: vfs.defs (server stubs for ext_server, #220)"
  VERBATIM
)

# ===========================================================================
# MIG-generated ahci_batch user stubs (batch device writes)
# ===========================================================================

set(AHCI_BATCH_DEFS ${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci/ahci_batch.defs)

set(AB_USER_C ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_user.c)
set(AB_USER_H ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch.h)

add_custom_command(
  OUTPUT ${AB_USER_C} ${AB_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci
    -header ${AB_USER_H}
    -sheader /dev/null
    -user ${AB_USER_C}
    -server /dev/null
    ${AHCI_BATCH_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${AHCI_BATCH_DEFS} ${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci/ahci_batch_types.h
  COMMENT "MIG: ahci_batch.defs (user stubs for ext2_server)"
  VERBATIM
)

# ===========================================================================
# MIG-generated device_master user stubs (for device_dma_alloc_sg)
# ===========================================================================

set(DEVICE_MASTER_DEFS ${UROS_UAPI_DIR}/device/device_master.defs)

set(DM_USER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_user.c)
set(DM_USER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master.h)

add_custom_command(
  OUTPUT ${DM_USER_C} ${DM_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DM_USER_H}
    -sheader /dev/null
    -user ${DM_USER_C}
    -server /dev/null
    ${DEVICE_MASTER_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEVICE_MASTER_DEFS}
  COMMENT "MIG: device_master.defs (user stubs for ext2_server)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(EXT2_SERVER_SOURCES
  ext_server.c
  device_read_overwrite.c
  nostdlib_stubs.c
  ${EXT2_SERVER_C}
  ${VFS_SERVER_C}
  ${AB_USER_C}
  ${DM_USER_C}
)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(ext_server_bin ${EXT2_SERVER_SOURCES})

add_dependencies(ext_server_bin mig_mach_public_headers)

target_compile_options(ext_server_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

option(OSFMK_EXT2_BENCH "Enable ext2 page cache benchmark at boot" OFF)

target_compile_definitions(ext_server_bin PRIVATE
  MACH_IPC_COMPAT=0
  $<$<BOOL:${OSFMK_EXT2_BENCH}>:EXT2_BENCH>
)

target_include_directories(ext_server_bin PRIVATE
  # MIG-generated headers (ext2fs_server_server.h, ahci_batch.h)
  ${CMAKE_CURRENT_BINARY_DIR}
  # ahci_batch_types.h
  ${CMAKE_SOURCE_DIR}/src/servers/block_device_server/ahci
  # ext2fs_types.h (imported by MIG-generated code)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # vfs_types.h (imported by MIG-generated vfs_server.c)
  ${CMAKE_SOURCE_DIR}/src/lib/libvfs
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/AT386
  # Kernel headers (mach/, device/, i386/)
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
  # libsa_fs headers (file_system.h, ext2fs/ext2fs.h, externs.h)
  ${CMAKE_SOURCE_DIR}/src/file_systems
  ${CMAKE_SOURCE_DIR}/src/file_systems/ext2fs
  # libcap.h for capability-gated device_open_cap
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  # libfspager.h for file-backed mmap (#276)
  ${CMAKE_SOURCE_DIR}/src/lib/libfspager
  # libgpu_console (#199 prep)
  ${CMAKE_SOURCE_DIR}/src/lib/libgpu_console
)

# Link against Mach userspace libraries + filesystem library.
target_link_libraries(ext_server_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_fs
  libblk
  libcap
  libfspager
  libnetname
  libflipc2
  libgpu_console
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(ext_server_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(ext_server_bin PROPERTIES
  OUTPUT_NAME "ext_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
