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
project(libdriver C)

# libdriver: what a module of chora is written against (#963).  It is linked
# into chora, whose symbols the modules resolve against, so the device master's
# and the HAL's client stubs are here and in no module.
#
# ⚠️ add_mig_userland and NOT scripts/mig, for irq_claim_test's reason: the
# wrapper sends a flag it does not know to the C preprocessor, and migcom then
# makes i386 stubs for an x86-64 program.

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

set(_DRV_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(DRV_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_DRV_OUT} DRV_MIG)
list(REMOVE_ITEM DRV_MIG_GENERATED ${_DRV_OUT}/device_master_server.c)

# The HAL's client half only, so that no `hal_server.h' of migcom's is made to
# shadow the HAL's own (#565).
set(DRV_HAL_GENERATED)
add_mig_userland_client(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                        ${_DRV_OUT} DRV_HAL)
list(APPEND DRV_MIG_GENERATED ${DRV_HAL_GENERATED})

add_library(libdriver STATIC libdriver.c ${DRV_MIG_GENERATED})

add_dependencies(libdriver mig_mach_public_headers)

target_compile_options(libdriver PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin -fPIC
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(libdriver PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(libdriver PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})

# ⚠️ The HAL's own directory before the generated one, as irq_claim_test says:
# migcom's names must not stand in front of the HAL's headers.
target_include_directories(libdriver PRIVATE
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${_DRV_OUT}
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

set_target_properties(libdriver PROPERTIES
  OUTPUT_NAME "driver"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
