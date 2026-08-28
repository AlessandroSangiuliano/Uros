cmake_minimum_required(VERSION 3.16)
project(libservice C)

# Generate MIG stubs from service.defs
set(SERVICE_DEFS "${UROS_UAPI_DIR}/servers/service.defs")
set(SERVICE_USER_C "${CMAKE_BINARY_DIR}/generated/service_user.c")
set(SERVICE_SERVER_C "${CMAKE_BINARY_DIR}/generated/service_server.c")
set(SERVICE_HEADER "${CMAKE_BINARY_DIR}/generated/service.h")

add_custom_command(
  OUTPUT ${SERVICE_USER_C} ${SERVICE_SERVER_C} ${SERVICE_HEADER}
  # #471: uapi first, and no pipe -- see cmake/mig.cmake for why both.
  COMMAND ${CMAKE_C_COMPILER} -E -x c
    -I${UROS_UAPI_DIR}
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
    ${SERVICE_DEFS} -o ${CMAKE_CURRENT_BINARY_DIR}/service.mig.i
  COMMAND $<TARGET_FILE:migcom>
    -header ${SERVICE_HEADER}
    -user ${SERVICE_USER_C}
    -server ${SERVICE_SERVER_C}
    ${CMAKE_CURRENT_BINARY_DIR}/service.mig.i
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${SERVICE_DEFS}
  COMMENT "MIG: service.defs"
  VERBATIM
)

# Library only contains the user-side MIG stub
add_library(libservice STATIC ${SERVICE_USER_C})

add_dependencies(libservice mig_mach_public_headers)

target_compile_options(libservice PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin  -include stdint.h>
)

target_include_directories(libservice PUBLIC
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # <string.h> lives in sa_mach/, not in the directory above it (#506,
  # same class as #495): without this root it came from the host's glibc.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libservice PROPERTIES
  OUTPUT_NAME "service"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
