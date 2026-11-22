# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# modload_test (#973): does libmodload know a module by its class.  A PIE that
# exports its symbols, as every program that loads modules is (chora's
# CMakeLists says why), and four modules in two pools of its own: a twin.so in
# modload_a and in modload_b, and only_a.so and only_b.so.  bootstrap's
# bundle carries them as modules/<class>/<name>, and chora.conf starts the
# test before chora_test.

cmake_minimum_required(VERSION 3.16)
project(modload_test C)

set(MODLOAD_TEST_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
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
  ${CMAKE_SOURCE_DIR}/include)

add_executable(modload_test_bin modload_test.c)

target_compile_options(modload_test_bin PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(modload_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(modload_test_bin PRIVATE ${MODLOAD_TEST_INCLUDE_DIRS})

target_link_libraries(modload_test_bin PRIVATE
  -Wl,--start-group
  libmodload
  libdl
  libpthreads
  libmach
  -Wl,--end-group)

target_link_options(modload_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--build-id=none)

add_dependencies(modload_test_bin libmodload libdl libpthreads libmach
                 mig_mach_public_headers)

set_target_properties(modload_test_bin PROPERTIES
  OUTPUT_NAME "modload_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")

# One module: its target, its source, its pool and its file name there.
function(modload_test_module target source class name)
  add_library(${target} SHARED ${source})
  target_compile_options(${target} PRIVATE
    -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -fPIC -fno-stack-protector)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "${name}"
    PREFIX ""
    SUFFIX ".so"
    LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--hash-style=sysv"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/${class}")
endfunction()

modload_test_module(modload_test_twin_a modules/twin_a.c modload_a twin)
modload_test_module(modload_test_twin_b modules/twin_b.c modload_b twin)
modload_test_module(modload_test_only_a modules/only_a.c modload_a only_a)
modload_test_module(modload_test_only_b modules/only_b.c modload_b only_b)
