cmake_minimum_required(VERSION 3.16)
project(act_test C)

# ===========================================================================
# Taking a thread out of a call it is not going to finish (#475)
# ===========================================================================
#
# x86-64 only, and that is the point rather than a limitation: i386 has walked
# its activation chain since before this tree existed, so a copy of this
# program there would test the 1990 kernel rather than the port.
#
# ⚠️ Like the name server, it cannot go through scripts/mig -- that wrapper
# sends any flag it does not know to the C preprocessor, so `-target x86_64'
# would never reach migcom and the stubs would come out with i386's message
# layout.  add_mig_userland calls migcom directly.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_E ${CMAKE_SOURCE_DIR}/export/include)
set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${_E}
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
    -I${_E}/mach_debug)

# exc.defs, for the server half only.  The task is the one being told about a
# fault, so what it needs is the demultiplexer and the three catch_ routines
# it calls; the user half would be the kernel's side of the same protocol and
# referencing it here would ask this program to implement the kernel's end.
set(_AT_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(AT_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/mach/exc.defs ${_AT_OUT} AT_MIG)
list(REMOVE_ITEM AT_MIG_GENERATED ${_AT_OUT}/exc_user.c)

add_executable(act_test_bin
  act_test.c ${AT_MIG_GENERATED})

target_compile_options(act_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin -w
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(act_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ ${_AT_OUT} first: the stubs include each other by quoted name.  The shared
# generated/ MIG directory is not needed here -- this test's stubs are its own.
target_include_directories(act_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${_AT_OUT}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/x86_64
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${_E}
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/include)

# --start-group because libpthreads and libmach reach into each other:
# libpthreads wants libmach's MIG stubs, libmach wants libpthreads' mig_support.
target_link_libraries(act_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(act_test_bin libmach libpthreads mig_mach_public_headers)

target_link_options(act_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(act_test_bin PROPERTIES
  OUTPUT_NAME "act_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
