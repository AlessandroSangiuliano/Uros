cmake_minimum_required(VERSION 3.16)
project(ahci_driver C)

# ===========================================================================
# MIG-generated device_master user stubs
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
  COMMENT "MIG: device_master.defs (user stubs for ahci_driver)"
  VERBATIM
)

# ===========================================================================
# MIG-generated ahci_batch server stubs (batch device writes)
# ===========================================================================

set(AHCI_BATCH_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/ahci_batch.defs)

set(AB_USER_C ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_user.c)
set(AB_USER_H ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch.h)
set(AB_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_server.c)
set(AB_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/ahci_batch_server.h)

add_custom_command(
  OUTPUT ${AB_USER_C} ${AB_USER_H} ${AB_SERVER_C} ${AB_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${AB_USER_H}
    -sheader ${AB_SERVER_H}
    -user ${AB_USER_C}
    -server ${AB_SERVER_C}
    ${AHCI_BATCH_DEFS}
  DEPENDS migcom ${AHCI_BATCH_DEFS} ${CMAKE_CURRENT_SOURCE_DIR}/ahci_batch_types.h
  COMMENT "MIG: ahci_batch.defs (batch write server stubs for ahci_driver)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(AHCI_SOURCES
  ahci_driver.c
  nostdlib_stubs.c
  ${DM_USER_C}
  ${AB_SERVER_C}
  ${CMAKE_BINARY_DIR}/generated/device_server.c
)

# Mark the generated device_server.c as generated so CMake doesn't
# complain about missing sources at configure time.
set_source_files_properties(
  ${CMAKE_BINARY_DIR}/generated/device_server.c
  PROPERTIES GENERATED TRUE
)

# Copy device_server.h to our local build dir so we can include it
# without adding generated/ to the include path (which would shadow
# the real mach.h with the MIG-generated one).
set(LOCAL_DEVICE_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/device_server.h)
add_custom_command(
  OUTPUT ${LOCAL_DEVICE_SERVER_H}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/generated/device_server.h
    ${LOCAL_DEVICE_SERVER_H}
  DEPENDS ${CMAKE_BINARY_DIR}/generated/device_server.h
  COMMENT "Copy device_server.h for ahci_driver"
)

# ===========================================================================
# Build target: Mach standalone executable
# ===========================================================================

add_executable(ahci_driver_server
  ${AHCI_SOURCES}
  ${LOCAL_DEVICE_SERVER_H}
)

target_compile_options(ahci_driver_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(ahci_driver_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(ahci_driver_server PRIVATE
  # MIG-generated headers (device_master.h, device_server.h, ahci_batch_server.h)
  ${CMAKE_CURRENT_BINARY_DIR}
  # ahci_batch_types.h
  ${CMAKE_CURRENT_SOURCE_DIR}
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
target_link_libraries(ahci_driver_server PRIVATE
  -Wl,--start-group
  libcthreads
  libmach
  libsa_mach
  libnetname
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(ahci_driver_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

# Ensure MIG-generated device server stubs are built before us.
add_dependencies(ahci_driver_server mig_device_header)

set_target_properties(ahci_driver_server PROPERTIES
  OUTPUT_NAME "ahci_driver"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
