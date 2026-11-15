# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# blk_write_label_test (#915): the block server's -W LABEL, a write right only
# for the partition whose ext2 volume is labelled LABEL.  In the bundle for a
# real machine that writes its boot log (bootstrap_bundle_log), not in ALL's.
cmake_minimum_required(VERSION 3.16)
project(blk_write_label_test C)

if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

add_executable(blk_write_label_test_bin blk_write_label_test.c)

add_dependencies(blk_write_label_test_bin mig_mach_public_headers)

target_compile_options(blk_write_label_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -Wall -Werror
  -include stdint.h)

target_compile_definitions(blk_write_label_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(blk_write_label_test_bin PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(blk_write_label_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libcap
  libmach
  -Wl,--end-group)

target_link_options(blk_write_label_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none)

set_target_properties(blk_write_label_test_bin PROPERTIES
  OUTPUT_NAME "blk_write_label_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")

# The manifest -- what this program is allowed to hold (#513).
set(BWL_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/blk_write_label_test.manifest)
set(BWL_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/blk_write_label_test.cmf")
add_custom_command(
  OUTPUT ${BWL_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${BWL_MANIFEST_OUT} ${BWL_MANIFEST_SRC}
  DEPENDS mkmanifest ${BWL_MANIFEST_SRC}
  COMMENT "mkmanifest: blk_write_label_test.manifest -> blk_write_label_test.cmf"
  VERBATIM)
add_custom_target(blk_write_label_test_manifest ALL DEPENDS ${BWL_MANIFEST_OUT})
add_dependencies(blk_write_label_test_bin blk_write_label_test_manifest)
