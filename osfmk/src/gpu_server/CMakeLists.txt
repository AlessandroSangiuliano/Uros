cmake_minimum_required(VERSION 3.16)
project(gpu_server C)

# ===========================================================================
# Common compile options (mirrors hal_server / cap_server)
# ===========================================================================

set(GPU_COMPILE_OPTIONS
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

set(GPU_INCLUDE_DIRS
  # MIG-generated headers (gpu_server.h, gpu_server_server.h) land here
  ${CMAKE_CURRENT_BINARY_DIR}
  # Internal header
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h (not used yet; reserved for #196 render thread)
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  # Pre-built export headers (gpu/, mach/, ...)
  ${CMAKE_SOURCE_DIR}/export/include
  # Kernel headers
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# ===========================================================================
# MIG: gpu_server.defs — server stubs for gpu_server, user stubs for clients
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(GPU_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/gpu_server.defs)

set(GPU_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/gpu_server_user.c)
set(GPU_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/gpu_server.h)
set(GPU_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/gpu_server_mig_impl.c)
set(GPU_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/gpu_server_mig.h)

add_custom_command(
  OUTPUT ${GPU_USER_C} ${GPU_USER_H} ${GPU_SERVER_C} ${GPU_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${GPU_USER_H}
    -sheader ${GPU_SERVER_H}
    -user ${GPU_USER_C}
    -server ${GPU_SERVER_C}
    ${GPU_DEFS}
  DEPENDS migcom ${GPU_DEFS}
  COMMENT "MIG: gpu_server.defs (server stubs for gpu_server)"
  VERBATIM
)

# ===========================================================================
# gpu_server executable
# ===========================================================================

set(GPU_SOURCES
  main.c
  core.c
  gpu_mig.c
  ${GPU_SERVER_C}
)

add_executable(gpu_server
  ${GPU_SOURCES}
  ${GPU_USER_H}     # marker — ensures MIG runs before compiling sources
  ${GPU_SERVER_H}
)

add_dependencies(gpu_server mig_mach_public_headers)

target_compile_options(gpu_server PRIVATE ${GPU_COMPILE_OPTIONS})

target_compile_definitions(gpu_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(gpu_server PRIVATE ${GPU_INCLUDE_DIRS})

# Same start/end-group dance as hal_server for circular libmach/libsa_mach
# resolution.  libnetname provides netname_check_in / netname_look_up.
target_link_libraries(gpu_server PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_mach
  libnetname
  -Wl,--end-group
)

target_link_options(gpu_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(gpu_server PROPERTIES
  OUTPUT_NAME "gpu_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
