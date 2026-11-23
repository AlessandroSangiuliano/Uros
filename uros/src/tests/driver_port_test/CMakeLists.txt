# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

# driver_port_test -- the driver's port, asked by a task that holds the master
# port and no device (#964).  x86-64, beside the HAL it asks for a line no
# device raises; no manifest, since it asks for no capability.
cmake_minimum_required(VERSION 3.16)
project(driver_port_test C)

if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
    -I${CMAKE_SOURCE_DIR}/src/servers/exec_server)

set(_DP_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(DP_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_DP_OUT} DP_MIG)
list(REMOVE_ITEM DP_MIG_GENERATED ${_DP_OUT}/device_master_server.c)
set(DP_HAL_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                        ${_DP_OUT} DP_HAL)
list(APPEND DP_MIG_GENERATED ${DP_HAL_GENERATED})
# #964: exec_server's client, for the image load the HAL will ask.
set(DP_EXEC_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/exec_server/exec.defs
                        ${_DP_OUT} DP_EXEC)
list(APPEND DP_MIG_GENERATED ${DP_EXEC_GENERATED})

add_executable(driver_port_test_bin driver_port_test.c ${DP_MIG_GENERATED})
target_compile_options(driver_port_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)
target_compile_definitions(driver_port_test_bin PRIVATE MACH_IPC_COMPAT=0)
target_include_directories(driver_port_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${CMAKE_SOURCE_DIR}/src/servers/exec_server
  ${_DP_OUT}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(driver_port_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libnetname
  -Wl,--end-group)
add_dependencies(driver_port_test_bin libmach libpthreads libnetname
                 mig_mach_public_headers)
target_link_options(driver_port_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)
set_target_properties(driver_port_test_bin PROPERTIES
  OUTPUT_NAME "driver_port_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")
