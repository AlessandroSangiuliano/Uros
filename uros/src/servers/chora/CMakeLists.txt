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
project(chora C)

# chora, one program for every device, and its first module (#963).

set(CHORA_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/lib/libdriver
  ${CMAKE_SOURCE_DIR}/src/lib/libmodload
  ${CMAKE_SOURCE_DIR}/src/lib/libdl
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${UROS_LIBPTHREADS_MD}
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/include)

# ===========================================================================
# The program
# ===========================================================================
#
# 🔑 A PIE that exports its symbols, as block_device_server is: a module is a
# shared object linked against nothing, and resolves libdriver's calls, and
# libmach's, against this program's own .dynsym (libmodload.c).  So libdriver
# goes in whole -- a call a module makes and chora does not is still there to
# be found -- and the program is exec'd, so exec_server relocates it (#963).

# #964: the server half of chora.defs, the control port each chora serves.
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel)
set(_CHORA_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(CHORA_MIG_GENERATED)
add_mig_userland_server(${CMAKE_CURRENT_SOURCE_DIR}/chora.defs ${_CHORA_OUT}
                        CHORA_MIG)

add_executable(chora_bin chora.c ${CHORA_MIG_GENERATED})

target_compile_options(chora_bin PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(chora_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(chora_bin PRIVATE ${CHORA_INCLUDE_DIRS} ${_CHORA_OUT})

target_link_libraries(chora_bin PRIVATE
  -Wl,--start-group
  -Wl,--whole-archive libdriver -Wl,--no-whole-archive
  libmodload
  libdl
  libpthreads
  libmach
  libnetname
  libcap
  -Wl,--end-group)

target_link_options(chora_bin PRIVATE
  ${UROS_ARCH_FLAGS} -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--build-id=none)

add_dependencies(chora_bin libdriver libmodload libdl libpthreads libmach
                 mig_mach_public_headers mkmanifest chora_probe_module)

set_target_properties(chora_bin PROPERTIES
  OUTPUT_NAME "chora"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")

# Its manifest goes into the program, as hello_exec's does: exec_server reads
# it there and derives the task's port from the parent's (#743).  The link
# depends on the manifest, so that a manifest edited alone is not left out.
set(CHORA_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/chora.manifest)
set(CHORA_CMF ${CMAKE_CURRENT_BINARY_DIR}/chora.cmf)
set_property(TARGET chora_bin APPEND PROPERTY LINK_DEPENDS ${CHORA_MANIFEST_SRC})

add_custom_command(TARGET chora_bin POST_BUILD
  COMMAND $<TARGET_FILE:mkmanifest> -o ${CHORA_CMF} ${CHORA_MANIFEST_SRC}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${CHORA_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            $<TARGET_FILE:chora_bin>
  COMMENT "embed .uros_manifest into chora"
  VERBATIM)

# ===========================================================================
# chora_probe: the test module for the network card, in the class `net'
# ===========================================================================
#
# No stack protector: its guard and its failure call are the program's to
# give, and a module this small has nothing to protect that is worth a stub.

add_library(chora_probe_module SHARED chora_probe/chora_probe.c)

add_dependencies(chora_probe_module mig_mach_public_headers)

target_compile_options(chora_probe_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(chora_probe_module PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(chora_probe_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(chora_probe_module PROPERTIES
  OUTPUT_NAME "chora_probe"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/net")

# ===========================================================================
# chora_absent: a module of the class `net' that no chora is told to load
# ===========================================================================
#
# With one module in a class, a chora that fetched its whole class would look
# like one that fetched its module; with this beside chora_probe it would load
# this one too (#963).  -init names the function libdl runs when it loads the
# file, which says WRONG: libdl calls DT_INIT and not .init_array.

add_library(chora_absent_module SHARED chora_absent/chora_absent.c)

add_dependencies(chora_absent_module mig_mach_public_headers)

target_compile_options(chora_absent_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(chora_absent_module PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(chora_absent_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(chora_absent_module PROPERTIES
  OUTPUT_NAME "chora_absent"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv -Wl,-init,chora_absent_loaded"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/net")

# ===========================================================================
# edu_probe: a module of the class `misc', for QEMU's educational device
# ===========================================================================
#
# edu has MSI with one message and no MSI-X table: the kind of device that got
# no message before #977.  chora_test starts a chora on it, where the board
# has one (run-x86_64.sh --edu).

# #964: a module that dies at its start, every time, on QEMU's pci-testdev
# (run-x86_64.sh --testdev): the HAL starts it again up to its limit, and
# chora_test counts the starts.
add_library(crash_probe_module SHARED crash_probe/crash_probe.c)

add_dependencies(crash_probe_module mig_mach_public_headers)

target_compile_options(crash_probe_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(crash_probe_module PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(crash_probe_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(crash_probe_module PROPERTIES
  OUTPUT_NAME "crash_probe"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/misc")

add_library(edu_probe_module SHARED edu_probe/edu_probe.c)

add_dependencies(edu_probe_module mig_mach_public_headers)

target_compile_options(edu_probe_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(edu_probe_module PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(edu_probe_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(edu_probe_module PROPERTIES
  OUTPUT_NAME "edu_probe"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/misc")

# #964: edu_probe again, named edu_next: the module chora_test hands the HAL to
# update edu's driver with.  It goes to user/updates, which make-disk-x86_64.sh
# writes to the disk's /mach_servers/updates and which is no pool: in the
# bundle's misc, or the disk's, it would declare edu alike with edu_probe, a
# tie the HAL does not break, and edu would get no driver.
add_library(edu_next_module SHARED edu_probe/edu_probe.c)

add_dependencies(edu_next_module mig_mach_public_headers)

target_compile_options(edu_next_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(edu_next_module PRIVATE MACH_IPC_COMPAT=0
  EDU_MODULE=edu_next)

target_include_directories(edu_next_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(edu_next_module PROPERTIES
  OUTPUT_NAME "edu_next"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/updates")

# #964: a module no chora could run, in the disk's pool misc: its name has 24
# characters, one past the room a driver's task name leaves a module's, and
# it declares the host bridge every board has.  The HAL must refuse it as it
# reads the pool, and say so, which run-x86_64.sh asks.  make-disk-x86_64.sh
# writes it to /mach_servers/modules/misc; no bundle holds it.
add_library(host_bridge_one_too_long_module SHARED
  host_bridge_one_too_long/host_bridge_one_too_long.c)

add_dependencies(host_bridge_one_too_long_module mig_mach_public_headers)

target_compile_options(host_bridge_one_too_long_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(host_bridge_one_too_long_module PRIVATE
  MACH_IPC_COMPAT=0)

target_include_directories(host_bridge_one_too_long_module PRIVATE
  ${CHORA_INCLUDE_DIRS})

set_target_properties(host_bridge_one_too_long_module PROPERTIES
  OUTPUT_NAME "host_bridge_one_too_long"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/misc")

# #964: and its other half, host_bridge_wrong_ops, in the same pool: a name
# with room in a driver's task name, its operations under another name than
# its own, and the host bridge declared.  The HAL must refuse it for its
# operations as it reads the pool, which run-x86_64.sh asks.
add_library(host_bridge_wrong_ops_module SHARED
  host_bridge_wrong_ops/host_bridge_wrong_ops.c)

add_dependencies(host_bridge_wrong_ops_module mig_mach_public_headers)

target_compile_options(host_bridge_wrong_ops_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(host_bridge_wrong_ops_module PRIVATE
  MACH_IPC_COMPAT=0)

target_include_directories(host_bridge_wrong_ops_module PRIVATE
  ${CHORA_INCLUDE_DIRS})

set_target_properties(host_bridge_wrong_ops_module PROPERTIES
  OUTPUT_NAME "host_bridge_wrong_ops"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/misc")

# #964: edu_probe a third time, named edu_again, in user/updates beside
# edu_next: the module chora_test [16] gives edu's stopped driver.  In no
# pool, so that its start, answered by edu_again, can only be the update's
# module, where edu_probe would also be the pools' choice for edu.
add_library(edu_again_module SHARED edu_probe/edu_probe.c)

add_dependencies(edu_again_module mig_mach_public_headers)

target_compile_options(edu_again_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(edu_again_module PRIVATE MACH_IPC_COMPAT=0
  EDU_MODULE=edu_again)

target_include_directories(edu_again_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(edu_again_module PROPERTIES
  OUTPUT_NAME "edu_again"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/updates")

# #964: crash_probe a second time, under its own name, with CRASH_UPDATE, as
# crash_probe_update.so in user/updates: the new build of the same module that
# chora_test [18] gives crash_probe's driver once the HAL has given it up.  In
# no pool; its line says it came from an update's bytes, which run-x86_64.sh
# counts at the starts the update begins, where the pool's would be the module
# the driver had.
add_library(crash_update_module SHARED crash_probe/crash_probe.c)

add_dependencies(crash_update_module mig_mach_public_headers)

target_compile_options(crash_update_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector
  -include stdint.h)

target_compile_definitions(crash_update_module PRIVATE MACH_IPC_COMPAT=0
  CRASH_UPDATE)

target_include_directories(crash_update_module PRIVATE ${CHORA_INCLUDE_DIRS})

set_target_properties(crash_update_module PROPERTIES
  OUTPUT_NAME "crash_probe_update"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/updates")
