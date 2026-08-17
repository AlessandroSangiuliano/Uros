cmake_minimum_required(VERSION 3.16)
project(fault_test C)

# ===========================================================================
# The three arms of #467, as a task (#467)
# ===========================================================================
#
# x86-64 only, and that is the point rather than a limitation: on i386 all
# three arms have worked since before this tree existed, and a copy of this
# program there would test the 1990 kernel rather than the port.  What #467
# delivers is the same three behaviours on a machine that had none of them,
# and this is what asks for them.
#
# ⚠️ Like the name server, it cannot go through scripts/mig -- that wrapper
# sends any flag it does not know to the C preprocessor, so `-target x86_64'
# would never reach migcom and the stubs would come out with i386's message
# layout.  add_mig_userland calls migcom directly.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

# exc.defs, for the server half only.  The task is the one being told about a
# fault, so what it needs is the demultiplexer and the three catch_ routines
# it calls; the user half would be the kernel's side of the same protocol and
# referencing it here would ask this program to implement the kernel's end.
set(_FT_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(FT_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/mach/exc.defs ${_FT_OUT} FT_MIG)
list(REMOVE_ITEM FT_MIG_GENERATED ${_FT_OUT}/exc_user.c)

add_executable(fault_test_bin
  fault_test.c ${FT_MIG_GENERATED})

target_compile_options(fault_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin -w
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(fault_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ ${_FT_OUT} first: the stubs include each other by quoted name.  The shared
# generated/ MIG directory is not needed here -- this test's stubs are its own.
target_include_directories(fault_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${_FT_OUT}
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
# libpthreads wants libmach's MIG stubs, libmach wants libpthreads' mig_support.
target_link_libraries(fault_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(fault_test_bin libmach libpthreads mig_mach_public_headers)

target_link_options(fault_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(fault_test_bin PROPERTIES
  OUTPUT_NAME "fault_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
