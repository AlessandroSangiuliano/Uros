cmake_minimum_required(VERSION 3.16)
project(exec_server C)

# ===========================================================================
# MIG-generated server stubs from exec.defs
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(EXEC_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/exec.defs)
set(EXEC_TYPES_H ${CMAKE_CURRENT_SOURCE_DIR}/exec_types.h)

set(EXEC_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/exec_user.c)
set(EXEC_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/exec.h)
set(EXEC_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/exec_server.c)
set(EXEC_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/exec_server.h)

add_custom_command(
  OUTPUT ${EXEC_USER_C} ${EXEC_USER_H} ${EXEC_SERVER_C} ${EXEC_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${EXEC_USER_H}
    -sheader ${EXEC_SERVER_H}
    -user ${EXEC_USER_C}
    -server ${EXEC_SERVER_C}
    ${EXEC_DEFS}
  DEPENDS migcom ${EXEC_DEFS} ${EXEC_TYPES_H}
  COMMENT "MIG: exec.defs (#228 v0.1.0)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(EXEC_SERVER_SOURCES
  exec_main.c
  exec_load.c
  exec_stack.c
  exec_vdso.c
  ${EXEC_SERVER_C}
)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(exec_server_bin
  ${EXEC_SERVER_SOURCES}
)

add_dependencies(exec_server_bin mig_mach_public_headers)

# Avoid colliding with the MIG-generated exec_server.c filename: the
# generated file goes in the build dir, the source-tree file
# exec_server.c (above) is the main.  CMake handles them by full path.
set_source_files_properties(${EXEC_SERVER_C} PROPERTIES GENERATED TRUE)

target_compile_options(exec_server_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(exec_server_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(exec_server_bin PRIVATE
  # MIG-generated headers (exec_server.h)
  ${CMAKE_CURRENT_BINARY_DIR}
  # exec_types.h, exec_internal.h
  ${CMAKE_CURRENT_SOURCE_DIR}
  # libelf.h
  ${CMAKE_SOURCE_DIR}/src/lib/libelf
  # libvfs.h, vfs_types.h
  ${CMAKE_SOURCE_DIR}/src/lib/libvfs
  # pthread.h
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/AT386
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h, servers/netname.h via export)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Pre-built export headers (mach/, servers/netname.h)
  ${CMAKE_SOURCE_DIR}/export/include
  # Kernel headers (mach/, device/, i386/)
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link against the Mach userspace libraries we need.
target_link_libraries(exec_server_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libnetname
  libcap
  libvfs
  libflipc2
  libelf
  -Wl,--end-group
)

target_link_options(exec_server_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(exec_server_bin PROPERTIES
  OUTPUT_NAME "exec_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
