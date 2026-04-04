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
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${EXT2_USER_H}
    -sheader ${EXT2_SERVER_H}
    -user ${EXT2_USER_C}
    -server ${EXT2_SERVER_C}
    ${EXT2_DEFS}
  DEPENDS migcom ${EXT2_DEFS}
  COMMENT "MIG: ext2fs_server.defs (server stubs for ext2_server)"
  VERBATIM
)

# ===========================================================================
# MIG-generated ahci_batch user stubs (batch device writes)
# ===========================================================================

set(AHCI_BATCH_DEFS ${CMAKE_SOURCE_DIR}/src/ahci_driver/ahci_batch.defs)

set(AB_USER_C ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_user.c)
set(AB_USER_H ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch.h)

add_custom_command(
  OUTPUT ${AB_USER_C} ${AB_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/ahci_driver
    -header ${AB_USER_H}
    -sheader /dev/null
    -user ${AB_USER_C}
    -server /dev/null
    ${AHCI_BATCH_DEFS}
  DEPENDS migcom ${AHCI_BATCH_DEFS} ${CMAKE_SOURCE_DIR}/src/ahci_driver/ahci_batch_types.h
  COMMENT "MIG: ahci_batch.defs (user stubs for ext2_server)"
  VERBATIM
)

# ===========================================================================
# MIG-generated device_master user stubs (for device_dma_alloc_sg)
# ===========================================================================

set(DEVICE_MASTER_DEFS ${CMAKE_SOURCE_DIR}/src/mach_kernel/device/device_master.defs)

set(DM_USER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_user.c)
set(DM_USER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master.h)

add_custom_command(
  OUTPUT ${DM_USER_C} ${DM_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DM_USER_H}
    -sheader /dev/null
    -user ${DM_USER_C}
    -server /dev/null
    ${DEVICE_MASTER_DEFS}
  DEPENDS migcom ${DEVICE_MASTER_DEFS}
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
  ${AB_USER_C}
  ${DM_USER_C}
)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(ext_server_bin ${EXT2_SERVER_SOURCES})

target_compile_options(ext_server_bin PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
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
  ${CMAKE_SOURCE_DIR}/src/ahci_driver
  # ext2fs_types.h (imported by MIG-generated code)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  # Pre-built export headers (mach/mach_interface.h, device/device_reply.h, servers/netname.h)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Kernel headers (mach/, device/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
  # libsa_fs headers (file_system.h, ext2fs/ext2fs.h, externs.h)
  ${CMAKE_SOURCE_DIR}/src/file_systems
  ${CMAKE_SOURCE_DIR}/src/file_systems/ext2fs
)

# Link against Mach userspace libraries + filesystem library.
target_link_libraries(ext_server_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_mach
  libsa_fs
  libblk
  libnetname
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(ext_server_bin PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(ext_server_bin PROPERTIES
  OUTPUT_NAME "ext_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
