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
  ${CMAKE_SOURCE_DIR}/src/proc_server
  ${CMAKE_BINARY_DIR}/generated/include
  # mach_services/include MUST come before build/generated so <mach.h>
  # resolves to the libmach userspace version, not the MIG-emitted shim.
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
# MIG: proc.defs — user stubs for proc_server RPCs (proc_set_ctty +
# proc_tcsetpgrp / proc_tcgetpgrp).  Wired in #275.1 so the tty class
# can claim the controlling terminal of the calling session.  Same
# pattern as device_master.defs above.
# ===========================================================================

set(PROC_DEFS   ${CMAKE_SOURCE_DIR}/src/proc_server/proc.defs)
set(PROC_USER_C ${CMAKE_CURRENT_BINARY_DIR}/proc_user.c)
set(PROC_USER_H ${CMAKE_CURRENT_BINARY_DIR}/proc.h)

add_custom_command(
  OUTPUT ${PROC_USER_C} ${PROC_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/proc_server
    -header ${PROC_USER_H}
    -sheader /dev/null
    -user ${PROC_USER_C}
    -server /dev/null
    ${PROC_DEFS}
  DEPENDS migcom ${PROC_DEFS}
  COMMENT "MIG: proc.defs (user stubs for char_server tty class)"
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
# Back-end modules — compiled as .so, loaded at runtime via libmodload
# from /mach_servers/modules/char/.  Symbol convention:
# `<name>_module_ops` of type char_module_ops_t.
# ===========================================================================

# --- PS/2 keyboard module (#206) ---
add_library(char_ps2_module SHARED
  modules/ps2.c
  modules/ssp_module_stub.c
)
add_dependencies(char_ps2_module mig_mach_public_headers)
target_compile_options(char_ps2_module PRIVATE ${CHAR_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(char_ps2_module PRIVATE MACH_IPC_COMPAT=0
  # Bring-up trace: log every byte read from PS2_DATA in ps2_irq_handler.
  # Remove when a real subscriber (tty / shell) consumes the events.
  PS2_DEBUG_TRACE)
target_include_directories(char_ps2_module PRIVATE ${CHAR_INCLUDE_DIRS})
set_target_properties(char_ps2_module PROPERTIES
  OUTPUT_NAME "ps2"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared -m32 -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules"
)

# --- PS/2 mouse module (#215) ---
add_library(char_ps2_mouse_module SHARED
  modules/ps2_mouse.c
  modules/ssp_module_stub.c
)
add_dependencies(char_ps2_mouse_module mig_mach_public_headers)
target_compile_options(char_ps2_mouse_module PRIVATE ${CHAR_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(char_ps2_mouse_module PRIVATE MACH_IPC_COMPAT=0
  # Bring-up trace: log every raw AUX byte from 8042.  Remove once a
  # real consumer (compositor / pointer cursor) reads the events.
  PS2_MOUSE_DEBUG_TRACE)
target_include_directories(char_ps2_mouse_module PRIVATE ${CHAR_INCLUDE_DIRS})
set_target_properties(char_ps2_mouse_module PROPERTIES
  OUTPUT_NAME "ps2_mouse"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared -m32 -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules"
)

# --- 16550 UART module (#207) ---
add_library(char_uart_module SHARED
  modules/uart.c
  modules/ssp_module_stub.c
)
add_dependencies(char_uart_module mig_mach_public_headers)
target_compile_options(char_uart_module PRIVATE ${CHAR_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(char_uart_module PRIVATE MACH_IPC_COMPAT=0
  # Bring-up trace: log every RX byte in uart_irq_handler.  Remove
  # when a real subscriber (shell / getty) consumes the stream.
  UART_DEBUG_TRACE)
target_include_directories(char_uart_module PRIVATE ${CHAR_INCLUDE_DIRS})
set_target_properties(char_uart_module PROPERTIES
  OUTPUT_NAME "uart"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared -m32 -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules"
)

# ===========================================================================
# char_server executable
# ===========================================================================

set(CHAR_SOURCES
  main.c
  core.c
  char_mig.c
  ${CHAR_SERVER_C}
  # core.c calls device_intr_register / device_intr_unregister
  # directly to wire module IRQs into the port set (#206).
  ${DM_USER_C}
  # #275.1: char_mig.c's tty_acquire_ctty handler RPCs proc_server.
  ${PROC_USER_C}
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
  ${DM_USER_H}
  ${PROC_USER_H}
)

add_dependencies(char_server mig_mach_public_headers libcap mig_cap_revoke
		 char_ps2_module char_uart_module char_ps2_mouse_module)

target_compile_options(char_server PRIVATE ${CHAR_COMPILE_OPTIONS} -fPIC)
target_compile_definitions(char_server PRIVATE MACH_IPC_COMPAT=0)
target_include_directories(char_server PRIVATE ${CHAR_INCLUDE_DIRS})

target_link_libraries(char_server PRIVATE
  -Wl,--start-group
  libmodload
  libdl
  libpthreads
  libmach
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
  -Wl,--build-id=none
)

set_target_properties(char_server PROPERTIES
  OUTPUT_NAME "char_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
