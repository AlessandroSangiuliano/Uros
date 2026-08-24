cmake_minimum_required(VERSION 3.16)
project(netname_test C)

# ===========================================================================
# A client for the name server, which is what #426's last clause asks for
# ===========================================================================
#
# x86-64 only.  On i386 every server is a netname client already, so a copy of
# this there would test the 1990 userland rather than the port -- the same
# reason act_test and cow_test are gated this way.
#
# ⚠️ No MIG of its own, unlike act_test.  The stubs this needs are netname's,
# libnetname already generates them for this target, and generating a second
# copy here would be two stubs from one .defs with nothing comparing them --
# which is the shape of #471 and #481 and is not worth repeating for four
# routines.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

add_executable(netname_test_bin netname_test.c)

target_compile_options(netname_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin 
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(netname_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(netname_test_bin PRIVATE
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

# --start-group for the same reason act_test needs it: libpthreads and libmach
# reach into each other.  libnetname joins the group because its stubs call
# libmach's mach_msg and libmach knows nothing of it.
target_link_libraries(netname_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libnetname
  -Wl,--end-group)
add_dependencies(netname_test_bin libmach libpthreads libnetname
                 mig_mach_public_headers)

target_link_options(netname_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(netname_test_bin PROPERTIES
  OUTPUT_NAME "netname_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
