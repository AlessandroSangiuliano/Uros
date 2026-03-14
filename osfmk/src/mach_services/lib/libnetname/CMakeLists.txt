cmake_minimum_required(VERSION 3.16)
project(libnetname C)

# Generate MIG stubs from netname.defs (via scripts/mig)
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(NETNAME_DEFS "${CMAKE_SOURCE_DIR}/export/powermac/include/servers/netname.defs")
set(NETNAME_USER_C "${CMAKE_BINARY_DIR}/generated/netname_user.c")
set(NETNAME_SERVER_C "${CMAKE_BINARY_DIR}/generated/netname_server.c")
set(NETNAME_USER_H "${CMAKE_BINARY_DIR}/generated/netname.h")
set(NETNAME_SERVER_H "${CMAKE_BINARY_DIR}/generated/netname_server.h")

add_custom_command(
  OUTPUT ${NETNAME_USER_C} ${NETNAME_SERVER_C} ${NETNAME_USER_H} ${NETNAME_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_services/servers/netname
    -header ${NETNAME_USER_H}
    -sheader ${NETNAME_SERVER_H}
    -user ${NETNAME_USER_C}
    -server ${NETNAME_SERVER_C}
    ${NETNAME_DEFS}
  DEPENDS migcom ${NETNAME_DEFS}
  COMMENT "MIG: netname.defs (libnetname)"
  VERBATIM
)

# Library only contains the user-side MIG stub
add_library(libnetname STATIC ${NETNAME_USER_C})

target_compile_options(libnetname PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686>
)

target_include_directories(libnetname PUBLIC
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/servers/netname
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libnetname PROPERTIES
  OUTPUT_NAME "netname"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
