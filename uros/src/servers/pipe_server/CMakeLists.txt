# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# pipe_server (#904): the witness of a pipe's ends.  One thread, one
# mach_msg_server loop: libmach and libnetname, and no pthread.

cmake_minimum_required(VERSION 3.16)
project(pipe_server C)

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(PIPE_DEFS    ${CMAKE_CURRENT_SOURCE_DIR}/pipe.defs)
set(PIPE_TYPES_H ${CMAKE_CURRENT_SOURCE_DIR}/pipe_types.h)

set(PIPE_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/pipe_user.c)
set(PIPE_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/pipe.h)
set(PIPE_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/pipe_server_mig.c)
set(PIPE_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/pipe_server.h)

add_custom_command(
  OUTPUT ${PIPE_USER_C} ${PIPE_USER_H} ${PIPE_SERVER_C} ${PIPE_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    ${UROS_MIG_TARGET_ARGS}
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${PIPE_USER_H}
    -sheader ${PIPE_SERVER_H}
    -user ${PIPE_USER_C}
    -server ${PIPE_SERVER_C}
    ${PIPE_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${PIPE_DEFS} ${PIPE_TYPES_H}
  COMMENT "MIG: pipe.defs (#904)"
  VERBATIM
)

# The server half of notify.defs, for the tokens' no-senders notifications:
# a program that demultiplexes an interface generates its half (#931).
set(NOTIFY_DEFS     ${UROS_UAPI_DIR}/mach/notify.defs)
set(NOTIFY_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/notify_user.c)
set(NOTIFY_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/notify.h)
set(NOTIFY_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/notify_server.c)
set(NOTIFY_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/notify_server.h)

add_custom_command(
  OUTPUT ${NOTIFY_USER_C} ${NOTIFY_USER_H} ${NOTIFY_SERVER_C} ${NOTIFY_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    ${UROS_MIG_TARGET_ARGS}
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${NOTIFY_USER_H}
    -sheader ${NOTIFY_SERVER_H}
    -user ${NOTIFY_USER_C}
    -server ${NOTIFY_SERVER_C}
    ${NOTIFY_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${NOTIFY_DEFS}
  COMMENT "MIG: notify.defs (no-senders for pipe_server)"
  VERBATIM
)

set_source_files_properties(${PIPE_SERVER_C} ${NOTIFY_SERVER_C}
  PROPERTIES GENERATED TRUE)

add_executable(pipe_server_bin
  pipe_server.c
  ${PIPE_SERVER_C}
  ${NOTIFY_SERVER_C}
)

add_dependencies(pipe_server_bin mig_mach_public_headers)

target_compile_options(pipe_server_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(pipe_server_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(pipe_server_bin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(pipe_server_bin PRIVATE
  -Wl,--start-group
  libmach
  libnetname
  -Wl,--end-group
)

target_link_options(pipe_server_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(pipe_server_bin PROPERTIES
  OUTPUT_NAME "pipe_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
