cmake_minimum_required(VERSION 3.16)
project(hello_server C)

# ===========================================================================
# Source files
# ===========================================================================

set(HELLO_SERVER_SOURCES
  hello_server.c
)

# ===========================================================================
# Build target: Mach standalone executable (same pattern as default_pager)
# ===========================================================================

add_executable(hello_server_server
  ${HELLO_SERVER_SOURCES}
)

add_dependencies(hello_server_server mig_mach_public_headers)

target_compile_options(hello_server_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(hello_server_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(hello_server_server PRIVATE
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
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
target_link_libraries(hello_server_server PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_mach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(hello_server_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(hello_server_server PROPERTIES
  OUTPUT_NAME "hello_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
