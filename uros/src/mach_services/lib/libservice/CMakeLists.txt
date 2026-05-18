cmake_minimum_required(VERSION 3.16)
project(libservice C)

# Generate MIG stubs from service.defs
set(SERVICE_DEFS "${CMAKE_SOURCE_DIR}/src/mach_services/include/servers/service.defs")
set(SERVICE_USER_C "${CMAKE_BINARY_DIR}/generated/service_user.c")
set(SERVICE_SERVER_C "${CMAKE_BINARY_DIR}/generated/service_server.c")
set(SERVICE_HEADER "${CMAKE_BINARY_DIR}/generated/service.h")

add_custom_command(
  OUTPUT ${SERVICE_USER_C} ${SERVICE_SERVER_C} ${SERVICE_HEADER}
  COMMAND ${CMAKE_C_COMPILER} -E -P -x c
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
    ${SERVICE_DEFS}
    | $<TARGET_FILE:migcom>
    -header ${SERVICE_HEADER}
    -user ${SERVICE_USER_C}
    -server ${SERVICE_SERVER_C}
  DEPENDS migcom ${SERVICE_DEFS}
  COMMENT "MIG: service.defs"
  VERBATIM
)

# Library only contains the user-side MIG stub
add_library(libservice STATIC ${SERVICE_USER_C})

add_dependencies(libservice mig_mach_public_headers)

target_compile_options(libservice PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -m32 -march=i686>
)

target_include_directories(libservice PUBLIC
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libservice PROPERTIES
  OUTPUT_NAME "service"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
