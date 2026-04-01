cmake_minimum_required(VERSION 3.16)
project(ipc_bench C)

# ===========================================================================
# MIG-generated ext2fs_server user stubs (for disk I/O benchmark)
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(EXT2_DEFS ${CMAKE_SOURCE_DIR}/src/ext_server/ext2fs_server.defs)

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
  COMMENT "MIG: ext2fs_server.defs (user stubs for ipc_bench)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(IPC_BENCH_SOURCES
  ipc_bench.c
  disk_bench.c
  mem_bench.c
  flipc2_bench.c
  flipc2_bench_common.c
  flipc2_bench_throughput.c
  flipc2_bench_rpc.c
  flipc2_bench_inter.c
  flipc2_bench_game.c
  flipc2_bench_endpoint.c
  nostdlib_stubs.c
  ${EXT2_USER_C}
)

# ===========================================================================
# Build target: Mach standalone executable (same pattern as default_pager)
# ===========================================================================

add_executable(ipc_bench_server
  ${IPC_BENCH_SOURCES}
)

target_compile_options(ipc_bench_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(ipc_bench_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(ipc_bench_server PRIVATE
  # MIG-generated headers (ext2fs_server.h)
  ${CMAKE_CURRENT_BINARY_DIR}
  # ext2fs_types.h (imported by MIG-generated code)
  ${CMAKE_SOURCE_DIR}/src/ext_server
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, cthreads.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Pre-built export headers (mach/mach_interface.h, device/device_reply.h, etc.)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Kernel headers (mach/, device/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link against Mach userspace libraries.
# Same circular dependency resolution as bootstrap: --start-group/--end-group.
target_link_libraries(ipc_bench_server PRIVATE
  -Wl,--start-group
  libcthreads
  libmach
  libsa_mach
  libnetname
  libflipc2
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(ipc_bench_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(ipc_bench_server PROPERTIES
  OUTPUT_NAME "ipc_bench"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
