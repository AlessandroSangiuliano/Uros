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
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(pci_function_test C)

# pci_function_test: a capability for one PCI function (#982).  The client
# halves of device_master.defs and hal.defs, as chora_test makes them --
# add_mig_userland, never scripts/mig, for their reason -- and cap_server's
# user stubs from libcap, for a capability asked on a child's port.
#
# x86-64 only: the edu devices it asks about are on run-x86_64.sh's --edu2.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

set(_PF_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(PF_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_PF_OUT} PF_MIG)
list(REMOVE_ITEM PF_MIG_GENERATED ${_PF_OUT}/device_master_server.c)

set(PF_HAL_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                        ${_PF_OUT} PF_HAL)
list(APPEND PF_MIG_GENERATED ${PF_HAL_GENERATED})

add_executable(pci_function_test_bin pci_function_test.c ${PF_MIG_GENERATED})

target_compile_options(pci_function_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(pci_function_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ The HAL's own directory before the generated one, for irq_claim_test's
# reason: migcom's names must not stand in front of the HAL's headers.
target_include_directories(pci_function_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${_PF_OUT}
  ${CMAKE_BINARY_DIR}/generated                  # cap_server.h, libcap's
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

target_link_libraries(pci_function_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libcap
  libnetname
  -Wl,--end-group)
add_dependencies(pci_function_test_bin libmach libpthreads libcap libnetname
                 mig_mach_public_headers)

target_link_options(pci_function_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(pci_function_test_bin PROPERTIES
  OUTPUT_NAME "pci_function_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")

# The manifest goes into the bundle as pci_function_test.cmf: bootstrap
# provisions a task from "<name>.cmf" by the name in its configuration line.
set(PF_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/pci_function_test.cmf")

add_custom_command(
  OUTPUT ${PF_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${PF_MANIFEST_OUT}
          ${CMAKE_CURRENT_SOURCE_DIR}/pci_function_test.manifest
  DEPENDS mkmanifest ${CMAKE_CURRENT_SOURCE_DIR}/pci_function_test.manifest
  COMMENT "mkmanifest: pci_function_test.manifest -> pci_function_test.cmf"
  VERBATIM
)

add_custom_target(pci_function_test_manifest ALL DEPENDS ${PF_MANIFEST_OUT})
add_dependencies(pci_function_test_bin pci_function_test_manifest)
