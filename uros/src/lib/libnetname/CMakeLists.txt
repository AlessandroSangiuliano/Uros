cmake_minimum_required(VERSION 3.16)
project(libnetname C)

# Generate MIG stubs from netname.defs (via scripts/mig)
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(NETNAME_DEFS "${UROS_UAPI_DIR}/servers/netname.defs")
set(NETNAME_USER_C "${CMAKE_BINARY_DIR}/generated/netname_user.c")
set(NETNAME_SERVER_C "${CMAKE_BINARY_DIR}/generated/netname_server.c")
set(NETNAME_USER_H "${CMAKE_BINARY_DIR}/generated/netname.h")
set(NETNAME_SERVER_H "${CMAKE_BINARY_DIR}/generated/netname_server.h")

add_custom_command(
  OUTPUT ${NETNAME_USER_C} ${NETNAME_SERVER_C} ${NETNAME_USER_H} ${NETNAME_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    # ⚠️ The target, which this could not pass until scripts/mig learned to
    # (#426): everything the wrapper did not recognise went to the C
    # PREPROCESSOR, so -target arrived at cc and migcom generated i386 layout
    # in silence.  ${UROS_MIG_TARGET_ARGS} is set at the top level, so it is
    # never empty here.
    ${UROS_MIG_TARGET_ARGS}
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_services/servers/netname
    -header ${NETNAME_USER_H}
    -sheader ${NETNAME_SERVER_H}
    -user ${NETNAME_USER_C}
    -server ${NETNAME_SERVER_C}
    ${NETNAME_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${NETNAME_DEFS}
  COMMENT "MIG: netname.defs (libnetname)"
  VERBATIM
)

# 🔥 PUBLISH THE STUB UNDER THE NAME ITS CONSUMERS INCLUDE (#481).
#
# Twenty-eight files write #include <servers/netname.h>, and until now that
# resolved to a copy of this very stub checked in under export/include -- a
# MIG output, from 1998, versioned.  It still matched byte for byte, which is
# exactly why nobody noticed: an edit to netname.defs regenerated this header
# and reached the four files that include it by the short name, while the
# other twenty-eight went on compiling against the checked-in one.
#
# ⚠️ Hung on mig_mach_public_headers, not on each consumer.  That target is
# the gate twenty of them already wait for, and #434 is what naming them one
# by one looks like when one gets forgotten: not a late header, a missing
# dependency edge that is silent until a build from an empty directory.
set(_NETNAME_PUB "${CMAKE_BINARY_DIR}/generated/include/servers")
file(MAKE_DIRECTORY "${_NETNAME_PUB}")
add_custom_command(
  OUTPUT ${_NETNAME_PUB}/netname.h
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${NETNAME_USER_H} ${_NETNAME_PUB}/netname.h
  DEPENDS ${NETNAME_USER_H}
  COMMENT "Publish servers/netname.h"
  VERBATIM
)
add_custom_target(netname_public_header DEPENDS ${_NETNAME_PUB}/netname.h)
add_dependencies(mig_mach_public_headers netname_public_header)

# Library only contains the user-side MIG stub
add_library(libnetname STATIC ${NETNAME_USER_C})

add_dependencies(libnetname mig_mach_public_headers)

target_compile_options(libnetname PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin  -mstack-protector-guard=global -include stdint.h>
)

# ⚠️ See the note in libcap/CMakeLists.txt: sa_mach is the only root here that
# has a <string.h>, and a target without it reaches the host's glibc one
# instead of failing.  libnetname builds nothing but MIG stubs, and those call
# memcpy -- so it was taking that declaration from the build machine.
target_include_directories(libnetname PUBLIC
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_services/servers/netname
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libnetname PROPERTIES
  OUTPUT_NAME "netname"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)

# PIC variant for the umbrella libc.so (#234 incr 2b).  Only Mach RPC stubs
# (no libc-level types) → a plain PIC clone is ABI-safe; flags/includes
# copied from the base target.
if(UROS_BUILD_MUSL)
  add_library(libnetname-pic STATIC ${NETNAME_USER_C})
  add_dependencies(libnetname-pic mig_mach_public_headers)
  set_target_properties(libnetname-pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(libnetname-pic PRIVATE
    $<TARGET_PROPERTY:libnetname,COMPILE_OPTIONS>)
  target_include_directories(libnetname-pic PRIVATE
    $<TARGET_PROPERTY:libnetname,INCLUDE_DIRECTORIES>)
  set_target_properties(libnetname-pic PROPERTIES OUTPUT_NAME "netname-pic")
endif()
