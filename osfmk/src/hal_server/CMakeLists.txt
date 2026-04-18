cmake_minimum_required(VERSION 3.16)
project(hal_server C)

# ===========================================================================
# Common compile options (mirrors block_device_server)
# ===========================================================================

set(HAL_COMPILE_OPTIONS
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

set(HAL_INCLUDE_DIRS
  # MIG-generated headers (device_master.h, hal_server_mig.h) land here
  ${CMAKE_CURRENT_BINARY_DIR}
  # Framework header + module headers
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/pci_scan
  # libdl + libmodload headers
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libdl
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libmodload
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  # Pre-built export headers
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Kernel headers
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# ===========================================================================
# MIG: device_master.defs — user stubs for pci_scan.so
# (so the module can call device_pci_config_read)
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
  COMMENT "MIG: device_master.defs (user stubs for hal_server)"
  VERBATIM
)

# ===========================================================================
# MIG: hal.defs — server stubs for hal_server
# ===========================================================================

set(HAL_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/hal.defs)

set(HAL_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/hal_user.c)
set(HAL_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/hal.h)
set(HAL_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/hal_server_mig_impl.c)
set(HAL_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/hal_server_mig.h)

add_custom_command(
  OUTPUT ${HAL_USER_C} ${HAL_USER_H} ${HAL_SERVER_C} ${HAL_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${HAL_USER_H}
    -sheader ${HAL_SERVER_H}
    -user ${HAL_USER_C}
    -server ${HAL_SERVER_C}
    ${HAL_DEFS}
  DEPENDS migcom ${HAL_DEFS}
  COMMENT "MIG: hal.defs (server stubs for hal_server)"
  VERBATIM
)

# ===========================================================================
# MIG: hal_notify.defs — user stubs (HAL sends async notifications to drivers)
# ===========================================================================

set(HAL_NOTIFY_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/hal_notify.defs)

set(HAL_NOTIFY_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/hal_notify_user.c)
set(HAL_NOTIFY_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/hal_notify.h)
set(HAL_NOTIFY_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/hal_notify_server.c)
set(HAL_NOTIFY_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/hal_notify_server.h)

add_custom_command(
  OUTPUT ${HAL_NOTIFY_USER_C} ${HAL_NOTIFY_USER_H}
         ${HAL_NOTIFY_SERVER_C} ${HAL_NOTIFY_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${HAL_NOTIFY_USER_H}
    -sheader ${HAL_NOTIFY_SERVER_H}
    -user ${HAL_NOTIFY_USER_C}
    -server ${HAL_NOTIFY_SERVER_C}
    ${HAL_NOTIFY_DEFS}
  DEPENDS migcom ${HAL_NOTIFY_DEFS}
  COMMENT "MIG: hal_notify.defs (user stubs for hal_server)"
  VERBATIM
)

# ===========================================================================
# Discovery modules — compiled as .so and loaded at runtime
# ===========================================================================

# --- pci_scan module ---
add_library(hal_pci_scan_module SHARED
  pci_scan/pci_scan.c
  ssp_module_stub.c
  ${DM_USER_C}
)
target_compile_options(hal_pci_scan_module PRIVATE ${HAL_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(hal_pci_scan_module PRIVATE MACH_IPC_COMPAT=0)
target_include_directories(hal_pci_scan_module PRIVATE ${HAL_INCLUDE_DIRS})
set_target_properties(hal_pci_scan_module PROPERTIES
  OUTPUT_NAME "pci_scan"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared -m32 -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules"
)

# ===========================================================================
# HAL server executable
# ===========================================================================

set(HAL_SOURCES
  hal_server.c
  hal_registry.c
  hal_driver_reg.c
  hal_mig.c
  nostdlib_stubs.c
  ${DM_USER_C}
  ${HAL_SERVER_C}
  ${HAL_NOTIFY_USER_C}
)

add_executable(hal_server
  ${HAL_SOURCES}
  ${HAL_USER_H}   # marker — ensures MIG runs before compiling sources
  ${HAL_SERVER_H}
  ${HAL_NOTIFY_USER_H}
)

add_dependencies(hal_server hal_pci_scan_module mig_device_header)

target_compile_options(hal_server PRIVATE ${HAL_COMPILE_OPTIONS} -fPIC)

target_compile_definitions(hal_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(hal_server PRIVATE ${HAL_INCLUDE_DIRS})

target_link_libraries(hal_server PRIVATE
  -Wl,--start-group
  libmodload
  libdl
  libpthreads
  libmach
  libsa_mach
  libnetname
  -Wl,--end-group
)

target_link_options(hal_server PRIVATE
  -m32 -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(hal_server PROPERTIES
  OUTPUT_NAME "hal_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
