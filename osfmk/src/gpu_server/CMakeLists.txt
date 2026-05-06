cmake_minimum_required(VERSION 3.16)
project(gpu_server C)

# ===========================================================================
# Common compile options (mirrors hal_server / block_device_server)
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
  # libdl + libmodload public headers
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libdl
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libmodload
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h (reserved for #196 render thread)
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
# MIG: device_master.defs — user stubs for back-end modules
# (vga.so calls device_mmio_map; future modules call dma_alloc /
# pci_config / intr_register).  Each .so links its own copy locally;
# the underlying mach_msg trap resolves through libmach in the host.
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(DEVICE_MASTER_DEFS ${CMAKE_SOURCE_DIR}/src/mach_kernel/device/device_master.defs)

set(DM_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/device_master_user.c)
set(DM_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/device_master.h)
set(DM_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_server.c)
set(DM_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master_server.h)

add_custom_command(
  OUTPUT ${DM_USER_C} ${DM_USER_H} ${DM_SERVER_C} ${DM_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DM_USER_H}
    -sheader ${DM_SERVER_H}
    -user ${DM_USER_C}
    -server ${DM_SERVER_C}
    ${DEVICE_MASTER_DEFS}
  DEPENDS migcom ${DEVICE_MASTER_DEFS}
  COMMENT "MIG: device_master.defs (user stubs for gpu_server modules)"
  VERBATIM
)

# ===========================================================================
# MIG: gpu_server.defs — server stubs for gpu_server, user stubs for clients
# ===========================================================================

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
# Back-end modules — compiled as .so, loaded at runtime via libmodload
# from /mach_servers/modules/gpu/.  Symbol convention: each module
# exports `<name>_module_ops` of type `gpu_module_ops_t`.
# ===========================================================================

# --- VGA module ---
add_library(gpu_vga_module SHARED
  modules/vga.c
  modules/ssp_module_stub.c
  ${DM_USER_C}
)
add_dependencies(gpu_vga_module mig_mach_public_headers)
target_compile_options(gpu_vga_module PRIVATE ${GPU_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(gpu_vga_module PRIVATE MACH_IPC_COMPAT=0)
target_include_directories(gpu_vga_module PRIVATE ${GPU_INCLUDE_DIRS})
set_target_properties(gpu_vga_module PROPERTIES
  OUTPUT_NAME "vga"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared -m32 -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules"
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

add_dependencies(gpu_server mig_mach_public_headers gpu_vga_module)

target_compile_options(gpu_server PRIVATE ${GPU_COMPILE_OPTIONS} -fPIC)

target_compile_definitions(gpu_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(gpu_server PRIVATE ${GPU_INCLUDE_DIRS})

# Same start/end-group dance as hal_server for circular libmach/libsa_mach
# resolution.  libmodload + libdl provide the runtime module loader;
# libnetname provides netname_check_in / netname_look_up.
target_link_libraries(gpu_server PRIVATE
  -Wl,--start-group
  libmodload
  libdl
  libpthreads
  libmach
  libsa_mach
  libnetname
  -Wl,--end-group
)

# PIE + --export-dynamic: required so dlopen'd modules can resolve
# host symbols (`gpu_device_port`, `printf`, ...) via the dynamic
# symbol table.  Mirrors hal_server / block_device_server.
target_link_options(gpu_server PRIVATE
  -m32 -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(gpu_server PROPERTIES
  OUTPUT_NAME "gpu_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
