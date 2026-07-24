cmake_minimum_required(VERSION 3.16)
project(proc_server C)

# ===========================================================================
# MIG-generated stubs from proc.defs (subsystem 3200, #237 v0.1.0)
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(PROC_DEFS  ${CMAKE_CURRENT_SOURCE_DIR}/proc.defs)
set(PROC_TYPES_H ${CMAKE_CURRENT_SOURCE_DIR}/proc_types.h)

set(PROC_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/proc_user.c)
set(PROC_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/proc.h)
set(PROC_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/proc_server.c)
set(PROC_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/proc_server.h)

add_custom_command(
  OUTPUT ${PROC_USER_C} ${PROC_USER_H} ${PROC_SERVER_C} ${PROC_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${PROC_USER_H}
    -sheader ${PROC_SERVER_H}
    -user ${PROC_USER_C}
    -server ${PROC_SERVER_C}
    ${PROC_DEFS}
  DEPENDS migcom ${PROC_DEFS} ${PROC_TYPES_H}
  COMMENT "MIG: proc.defs (#237 v0.1.0)"
  VERBATIM
)

# ===========================================================================
# MIG-generated server stubs from vfs.defs (proc_server hosts /proc)
# ===========================================================================

set(VFS_DEFS  ${CMAKE_SOURCE_DIR}/src/lib/libvfs/vfs.defs)
set(VFS_TYPES ${CMAKE_SOURCE_DIR}/src/lib/libvfs/vfs_types.h)

set(VFS_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/vfs_user.c)
set(VFS_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/vfs.h)
set(VFS_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.c)
set(VFS_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.h)

add_custom_command(
  OUTPUT ${VFS_USER_C} ${VFS_USER_H} ${VFS_SERVER_C} ${VFS_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/lib/libvfs
    -header ${VFS_USER_H}
    -sheader ${VFS_SERVER_H}
    -user ${VFS_USER_C}
    -server ${VFS_SERVER_C}
    ${VFS_DEFS}
  DEPENDS migcom ${VFS_DEFS} ${VFS_TYPES}
  COMMENT "MIG: vfs.defs (server stubs for proc_server / proc)"
  VERBATIM
)

# ===========================================================================
# MIG-generated server stubs from notify.defs (DEAD_NAME handler)
# ===========================================================================

set(NOTIFY_DEFS  ${CMAKE_SOURCE_DIR}/export/include/mach/notify.defs)
set(NOTIFY_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/notify_user.c)
set(NOTIFY_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/notify.h)
set(NOTIFY_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/notify_server.c)
set(NOTIFY_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/notify_server.h)

add_custom_command(
  OUTPUT ${NOTIFY_USER_C} ${NOTIFY_USER_H} ${NOTIFY_SERVER_C} ${NOTIFY_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${NOTIFY_USER_H}
    -sheader ${NOTIFY_SERVER_H}
    -user ${NOTIFY_USER_C}
    -server ${NOTIFY_SERVER_C}
    ${NOTIFY_DEFS}
  DEPENDS migcom ${NOTIFY_DEFS}
  COMMENT "MIG: notify.defs (DEAD_NAME handler for proc_server)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(PROC_SERVER_SOURCES
  proc_main.c
  ${PROC_SERVER_C}
  ${VFS_SERVER_C}
  ${VFS_USER_C}
  ${NOTIFY_SERVER_C}
)

# Avoid the same exec_server-style collision (MIG output proc_server.c
# vs hypothetical source proc_server.c — the source is named proc_main.c
# so this is just defensive).
set_source_files_properties(${PROC_SERVER_C} PROPERTIES GENERATED TRUE)
set_source_files_properties(${VFS_SERVER_C} PROPERTIES GENERATED TRUE)
set_source_files_properties(${VFS_USER_C} PROPERTIES GENERATED TRUE)
set_source_files_properties(${NOTIFY_SERVER_C} PROPERTIES GENERATED TRUE)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(proc_server_bin
  ${PROC_SERVER_SOURCES}
)

add_dependencies(proc_server_bin mig_mach_public_headers)

target_compile_options(proc_server_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(proc_server_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(proc_server_bin PRIVATE
  # MIG-generated headers
  ${CMAKE_CURRENT_BINARY_DIR}
  # proc_types.h (imported)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # vfs_types.h (imported by vfs.defs)
  ${CMAKE_SOURCE_DIR}/src/lib/libvfs
  # pthread.h
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/AT386
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Pre-built export headers (mach/, servers/netname.h)
  ${CMAKE_SOURCE_DIR}/export/include
  # Kernel headers
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(proc_server_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libnetname
  -Wl,--end-group
)

target_link_options(proc_server_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(proc_server_bin PROPERTIES
  OUTPUT_NAME "proc_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
