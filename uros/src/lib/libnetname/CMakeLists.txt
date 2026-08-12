cmake_minimum_required(VERSION 3.16)
project(libnetname C)

# Generate MIG stubs from netname.defs (via scripts/mig)
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(NETNAME_DEFS "${CMAKE_SOURCE_DIR}/export/include/servers/netname.defs")
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
    -I${CMAKE_SOURCE_DIR}/export/include
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

# Library only contains the user-side MIG stub
add_library(libnetname STATIC ${NETNAME_USER_C})

# ⚠️ The target that publishes the MIG headers has one name per architecture.
if(UROS_TARGET_ARCH STREQUAL "i386")
  add_dependencies(libnetname mig_mach_public_headers)
else()
  add_dependencies(libnetname mig_mach_headers_x86_64)
endif()

target_compile_options(libnetname PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h>
)

target_include_directories(libnetname PUBLIC
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/servers/netname
  ${CMAKE_SOURCE_DIR}/export/include
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
