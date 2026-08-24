cmake_minimum_required(VERSION 3.16)
project(sig_test C)

# Userspace exerciser for proc_server v0.2.0 signal infrastructure
# (#238).  Acts as a proc.defs MIG client, so it needs the user
# stubs generated locally — proc_server itself produces only the
# server stubs.

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(PROC_DEFS  ${CMAKE_SOURCE_DIR}/src/servers/proc_server/proc.defs)
set(PROC_USER_C ${CMAKE_CURRENT_BINARY_DIR}/proc_user.c)
set(PROC_USER_H ${CMAKE_CURRENT_BINARY_DIR}/proc.h)

add_custom_command(
  OUTPUT ${PROC_USER_C} ${PROC_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/proc_server
    -header ${PROC_USER_H}
    -sheader /dev/null
    -user ${PROC_USER_C}
    -server /dev/null
    ${PROC_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${PROC_DEFS}
  COMMENT "MIG: proc.defs (user stubs for sig_test)"
  VERBATIM
)

add_executable(sig_test_bin
  sig_test.c
  ${PROC_USER_C}
)

add_dependencies(sig_test_bin mig_mach_public_headers)

target_compile_options(sig_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  
  -include stdint.h
)

target_compile_definitions(sig_test_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(sig_test_bin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/src/servers/proc_server
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(sig_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libmach
  -Wl,--end-group
)

target_link_options(sig_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(sig_test_bin PROPERTIES
  OUTPUT_NAME "sig_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
