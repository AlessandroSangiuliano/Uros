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
    ${UROS_MIG_TARGET_ARGS}
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${EP_USER_H}
    -sheader ${EP_SERVER_H}
    -user ${EP_USER_C}
    -server ${EP_SERVER_C}
    ${EP_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${EP_DEFS} ${CMAKE_CURRENT_SOURCE_DIR}/flipc2_endpoint_types.h
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
  flipc2_pollset.c
  ${EP_USER_C}
  ${EP_SERVER_C}
)

add_library(libflipc2 STATIC ${LIB_SOURCES})

add_dependencies(libflipc2 mig_device_header mig_mach_public_headers)

target_compile_options(libflipc2 PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin  -include stdint.h -mstack-protector-guard=global>
)

target_include_directories(libflipc2 PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # <string.h> lives in sa_mach/, not in the directory above it (#506,
  # same class as #495): without this root it came from the host's glibc.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libflipc2 PROPERTIES
  OUTPUT_NAME "flipc2"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)

# ---------------------------------------------------------------------------
# The channel code, PIC, for the umbrella libc.so (#904).
#
# libc.so's libposix-uros makes pipes, and a pipe is a FLIPC channel: it
# creates one and lets go of it, and the produce and consume paths are the
# inline functions of flipc2.h.  So this is flipc2_channel.c alone, and libc.so
# still carries no endpoint, buffer group or pollset, which are servers' and
# reach for what a program of musl has no business with.  Built as libvfs-pic
# is, the other FLIPC user in libc.so: musl's headers first, no stack
# protector, position independent.  A static program's libc.a takes the whole
# libflipc2 already.
# ---------------------------------------------------------------------------
if(UROS_BUILD_MUSL)
  add_library(libflipc2-pic STATIC flipc2_channel.c)
  add_dependencies(libflipc2-pic mig_device_header mig_mach_public_headers
                   musl_extproj)
  set_target_properties(libflipc2-pic PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "flipc2-pic")
  target_compile_options(libflipc2-pic PRIVATE
    -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -ffreestanding
    -fno-stack-protector -include stdint.h
  )
  target_include_directories(libflipc2-pic PRIVATE
    ${CMAKE_BINARY_DIR}/src/contrib/musl-install/include   # musl headers FIRST
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated/include
    ${CMAKE_SOURCE_DIR}/src/mach_services/include
    ${UROS_ARCH_INCLUDE}
    ${CMAKE_SOURCE_DIR}/src/mach_kernel
    ${CMAKE_SOURCE_DIR}/include
  )
endif()
