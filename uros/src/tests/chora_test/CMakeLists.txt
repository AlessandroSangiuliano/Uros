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

cmake_minimum_required(VERSION 3.16)
project(chora_test C)

# chora_test: starts chora on the network card twice (#963).  The client
# halves of device_master.defs, hal.defs and exec.defs, as irq_claim_test and
# cap_test make them -- add_mig_userland, never scripts/mig, for their reason.

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
    -I${CMAKE_SOURCE_DIR}/src/servers/exec_server)

set(_CT_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(CT_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_CT_OUT} CT_MIG)
list(REMOVE_ITEM CT_MIG_GENERATED ${_CT_OUT}/device_master_server.c)

set(CT_HAL_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                        ${_CT_OUT} CT_HAL)
list(APPEND CT_MIG_GENERATED ${CT_HAL_GENERATED})

set(CT_EXEC_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/exec_server/exec.defs
                        ${_CT_OUT} CT_EXEC)
list(APPEND CT_MIG_GENERATED ${CT_EXEC_GENERATED})

add_executable(chora_test_bin chora_test.c ${CT_MIG_GENERATED})

target_compile_options(chora_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(chora_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ The HAL's own directory before the generated one, for irq_claim_test's
# reason: migcom's names must not stand in front of the HAL's headers.
target_include_directories(chora_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${CMAKE_SOURCE_DIR}/src/lib/libdriver          # chora_given.h, #964
  ${CMAKE_SOURCE_DIR}/src/servers/exec_server
  ${_CT_OUT}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(chora_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libcap
  libnetname
  -Wl,--end-group)
add_dependencies(chora_test_bin libmach libpthreads mig_mach_public_headers)

target_link_options(chora_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(chora_test_bin PROPERTIES
  OUTPUT_NAME "chora_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")

# The manifest goes into the bundle as chora_test.cmf: bootstrap provisions a
# task from "<name>.cmf" by the name in its configuration line.
set(CT_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/chora_test.cmf")

add_custom_command(
  OUTPUT ${CT_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${CT_MANIFEST_OUT}
          ${CMAKE_CURRENT_SOURCE_DIR}/chora_test.manifest
  DEPENDS mkmanifest ${CMAKE_CURRENT_SOURCE_DIR}/chora_test.manifest
  COMMENT "mkmanifest: chora_test.manifest -> chora_test.cmf"
  VERBATIM
)

add_custom_target(chora_test_manifest ALL DEPENDS ${CT_MANIFEST_OUT})
add_dependencies(chora_test_bin chora_test_manifest)
