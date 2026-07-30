cmake_minimum_required(VERSION 3.16)
project(gpustat C)

# MIG: client stubs for gpu_server.defs (gpu_query_stats).
set(MIG_SCRIPT   ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(GPU_DEFS     ${CMAKE_SOURCE_DIR}/src/servers/gpu_server/gpu_server.defs)
set(GPU_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/gpustat_gpu_user.c)
set(GPU_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/gpu_server.h)

add_custom_command(
  OUTPUT ${GPU_USER_C} ${GPU_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${GPU_USER_H}
    -user ${GPU_USER_C}
    ${GPU_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${GPU_DEFS}
  COMMENT "MIG: gpu_server.defs (user stubs for gpustat)"
  VERBATIM
)

add_executable(gpustat_bin
  gpustat.c
  ${GPU_USER_C}
  ${GPU_USER_H}
)

add_dependencies(gpustat_bin mig_mach_public_headers)

target_compile_options(gpustat_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(gpustat_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(gpustat_bin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}                          # gpu_server.h (MIG)
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
)

target_link_libraries(gpustat_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libcap
  libmach
  -Wl,--end-group
)

target_link_options(gpustat_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(gpustat_bin PROPERTIES
  OUTPUT_NAME "gpustat"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
