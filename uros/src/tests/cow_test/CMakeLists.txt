cmake_minimum_required(VERSION 3.16)
project(cow_test C)

# ===========================================================================
# The fifth clause of #407's done-when, as a task (#407)
# ===========================================================================
#
# x86-64 only, and for the same reason fault_test is: on i386 a fork with
# copy-on-write has worked since before this tree existed, so a copy of this
# program there would be testing the 1990 kernel rather than the port.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

# No MIG of its own, unlike fault_test.  Every call this program makes --
# vm_allocate, vm_inherit, vm_read_overwrite, task_create, task_terminate --
# is one libmach already provides, and it implements no server protocol, so
# there is no .defs whose server half would belong to it.
add_executable(cow_test_bin cow_test.c)

# ⚠️ No -w, unlike its neighbours.  fault_test and act_test carry it inherited
# from the servers they were modelled on; a file written today has no excuse to
# start with its warnings switched off, and this one compiles clean without it.
target_compile_options(cow_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin -Wall -Wextra
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(cow_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(cow_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/x86_64
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/include)

# --start-group because libpthreads and libmach reach into each other:
# libpthreads wants libmach's MIG stubs, libmach wants libpthreads'
# mig_support.  This program has one thread, but the entry point comes from
# libmach's crt0 and that is the pair it was linked against.
target_link_libraries(cow_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(cow_test_bin libmach libpthreads mig_mach_public_headers)

# 🔥 Linked as a static PIE (#422), and that is not about this test.
#
# The bootstrap loader's relocation pass had never been executed on this
# target: every image in the bundle was ET_EXEC, so the code that applies
# RELATIVE relocations was written, linked and never run -- and on x86-64 it
# was 32-bit-only, which fails by finding no DT_REL and reporting success.
#
# The fleet (#429) starts with cap_server, which on i386 is ET_DYN with 446
# relocations.  Rather than wait for it to arrive and discover the loader
# there, one image already in the bundle is built the way it will be.  If the
# relocations are applied wrongly this test does not report a wrong answer, it
# faults before printing anything -- which is exactly the property wanted from
# a subject.
target_link_options(cow_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -pie -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--hash-style=sysv
  -Wl,--build-id=none)

set_target_properties(cow_test_bin PROPERTIES
  OUTPUT_NAME "cow_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
