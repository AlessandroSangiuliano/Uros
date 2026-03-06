cmake_minimum_required(VERSION 3.16)
project(ipc_bench C)

# ===========================================================================
# Source files
# ===========================================================================

set(IPC_BENCH_SOURCES
  ipc_bench.c
  nostdlib_stubs.c
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
