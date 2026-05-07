cmake_minimum_required(VERSION 3.16)
project(char_server C)

# Mirror gpu_server CMakeLists structure.

set(CHAR_COMPILE_OPTIONS
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

set(CHAR_INCLUDE_DIRS
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libdl
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libmodload
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libcap
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libgpu_console
  ${CMAKE_BINARY_DIR}/generated/include
  # mach_services/include MUST come before build/generated so <mach.h>
  # resolves to the libsa_mach version, not the MIG-emitted shim.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# ===========================================================================
# MIG: device_master.defs — user stubs for back-end modules
# (ps2.so will use device_intr_register; uart.so the same).
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
  COMMENT "MIG: device_master.defs (user stubs for char_server modules)"
  VERBATIM
)

# ===========================================================================
# MIG: char_server.defs — server stubs for char_server, user stubs for clients
# ===========================================================================

set(CHAR_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/char_server.defs)

set(CHAR_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/char_server_user.c)
set(CHAR_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/char_server.h)
set(CHAR_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/char_server_mig_impl.c)
set(CHAR_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/char_server_mig.h)

add_custom_command(
  OUTPUT ${CHAR_USER_C} ${CHAR_USER_H} ${CHAR_SERVER_C} ${CHAR_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${CHAR_USER_H}
    -sheader ${CHAR_SERVER_H}
    -user ${CHAR_USER_C}
    -server ${CHAR_SERVER_C}
    ${CHAR_DEFS}
  DEPENDS migcom ${CHAR_DEFS}
  COMMENT "MIG: char_server.defs (server stubs for char_server)"
  VERBATIM
)

# ===========================================================================
# char_server executable
# ===========================================================================

set(CHAR_SOURCES
  main.c
  core.c
  char_mig.c
  ${CHAR_SERVER_C}
  ${CMAKE_BINARY_DIR}/generated/cap_revoke_server.c
)

set_source_files_properties(
  ${CMAKE_BINARY_DIR}/generated/cap_revoke_server.c
  ${CMAKE_BINARY_DIR}/generated/cap_revoke_server.h
  PROPERTIES GENERATED TRUE
)

add_executable(char_server
  ${CHAR_SOURCES}
  ${CHAR_USER_H}
  ${CHAR_SERVER_H}
)

add_dependencies(char_server mig_mach_public_headers libcap mig_cap_revoke)

target_compile_options(char_server PRIVATE ${CHAR_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(char_server PRIVATE MACH_IPC_COMPAT=0)
target_include_directories(char_server PRIVATE ${CHAR_INCLUDE_DIRS})

target_link_libraries(char_server PRIVATE
  -Wl,--start-group
  libmodload
  libdl
  libpthreads
  libmach
  libsa_mach
  libnetname
  libcap
  libgpu_console
  -Wl,--end-group
)

target_link_options(char_server PRIVATE
  -m32 -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(char_server PROPERTIES
  OUTPUT_NAME "char_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
