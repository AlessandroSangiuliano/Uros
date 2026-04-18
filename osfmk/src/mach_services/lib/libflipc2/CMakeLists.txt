cmake_minimum_required(VERSION 3.16)
project(libflipc2 C)

# ===========================================================================
# MIG: flipc2_endpoint.defs
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(EP_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/flipc2_endpoint.defs)

set(EP_USER_C ${CMAKE_CURRENT_BINARY_DIR}/flipc2_endpoint_user.c)
set(EP_USER_H ${CMAKE_CURRENT_BINARY_DIR}/flipc2_endpoint.h)
set(EP_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/flipc2_endpoint_server.c)
set(EP_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/flipc2_endpoint_server.h)

add_custom_command(
  OUTPUT ${EP_USER_C} ${EP_USER_H} ${EP_SERVER_C} ${EP_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${EP_USER_H}
    -sheader ${EP_SERVER_H}
    -user ${EP_USER_C}
    -server ${EP_SERVER_C}
    ${EP_DEFS}
  DEPENDS migcom ${EP_DEFS} ${CMAKE_CURRENT_SOURCE_DIR}/flipc2_endpoint_types.h
  COMMENT "MIG: flipc2_endpoint.defs"
  VERBATIM
)

# ===========================================================================
# Library sources
# ===========================================================================

set(LIB_SOURCES
  flipc2_channel.c
  flipc2_bufgroup.c
  flipc2_endpoint.c
  ${EP_USER_C}
  ${EP_SERVER_C}
)

add_library(libflipc2 STATIC ${LIB_SOURCES})

add_dependencies(libflipc2 mig_device_header mig_mach_public_headers)

target_compile_options(libflipc2 PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -m32 -march=i686 -mstack-protector-guard=global>
)

target_include_directories(libflipc2 PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libflipc2 PROPERTIES
  OUTPUT_NAME "flipc2"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
