cmake_minimum_required(VERSION 3.16)
project(notify_forge_test C)

# ===========================================================================
# A notification the kernel did not send is refused (#981)
# ===========================================================================
#
# x86-64 only, as mount_test: the HAL, pipe_server and libflipc2's endpoints
# are the receivers asked, and pipe_server is not on i386.
#
# Its own MIG: the user half of pipe.defs, for the pipe it registers to hold a
# token of -- as libposix-uros generates it for posix_pipe.c, under another
# header name, pipe_user.h.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(PIPE_DEFS   ${CMAKE_SOURCE_DIR}/src/servers/pipe_server/pipe.defs)
set(PIPE_USER_C ${CMAKE_CURRENT_BINARY_DIR}/pipe_user.c)
set(PIPE_USER_H ${CMAKE_CURRENT_BINARY_DIR}/pipe_user.h)

add_custom_command(
  OUTPUT ${PIPE_USER_C} ${PIPE_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    ${UROS_MIG_TARGET_ARGS}
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/servers/pipe_server
    -header ${PIPE_USER_H}
    -sheader /dev/null
    -user ${PIPE_USER_C}
    -server /dev/null
    ${PIPE_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${PIPE_DEFS}
  COMMENT "MIG: pipe.defs (notify_forge_test)"
  VERBATIM
)
set_source_files_properties(${PIPE_USER_C} ${PIPE_USER_H} PROPERTIES GENERATED TRUE)

add_executable(notify_forge_test_bin notify_forge_test.c ${PIPE_USER_C})

target_compile_options(notify_forge_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -msse -msse2 -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(notify_forge_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(notify_forge_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}                    # pipe_user.h
  ${CMAKE_SOURCE_DIR}/src/servers/pipe_server    # pipe_types.h
  ${CMAKE_BINARY_DIR}/src/lib/libflipc2          # flipc2_endpoint.h
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

# The group because libpthreads and libmach reach into each other, and the
# libraries above them into both.
target_link_libraries(notify_forge_test_bin PRIVATE
  -Wl,--start-group
  libflipc2
  libnetname
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(notify_forge_test_bin libflipc2 libnetname libpthreads libmach
                 mig_mach_public_headers)

target_link_options(notify_forge_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(notify_forge_test_bin PROPERTIES
  OUTPUT_NAME "notify_forge_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")
