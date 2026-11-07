# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# bootlog (#915): the kernel's log, klog, read with host_get_log() and written
# to DIR/boot-N.txt -- for a real machine that clears its memory when it
# resets.  In bootstrap_bundle_log, not in ALL's bundle.
cmake_minimum_required(VERSION 3.16)
project(bootlog C)

if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

add_executable(bootlog_bin bootlog.c)

add_dependencies(bootlog_bin mig_mach_public_headers)

target_compile_options(bootlog_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -Wall -Werror
  -include stdint.h)

target_compile_definitions(bootlog_bin PRIVATE MACH_IPC_COMPAT=0)

target_link_libraries(bootlog_bin PRIVATE
  -Wl,--start-group
  libvfs
  libflipc2
  libnetname
  libpthreads
  libmach
  -Wl,--end-group)

target_link_options(bootlog_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none)

set_target_properties(bootlog_bin PROPERTIES
  OUTPUT_NAME "bootlog"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")
