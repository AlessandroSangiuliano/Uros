cmake_minimum_required(VERSION 3.16)
project(libgpu_console C)

# ===========================================================================
# MIG: gpu_server.defs (user stubs only — we are a client)
# ===========================================================================

set(MIG_SCRIPT  ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(GPU_DEFS    ${CMAKE_SOURCE_DIR}/src/gpu_server/gpu_server.defs)
set(GPU_USER_C  ${CMAKE_BINARY_DIR}/generated/gpu_console_gpu_user.c)
set(GPU_USER_H  ${CMAKE_BINARY_DIR}/generated/gpu_console_gpu.h)

add_custom_command(
  OUTPUT ${GPU_USER_C} ${GPU_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${GPU_USER_H}
    -user ${GPU_USER_C}
    ${GPU_DEFS}
  DEPENDS migcom ${GPU_DEFS}
  COMMENT "MIG: gpu_server.defs (user stubs for libgpu_console)"
  VERBATIM
)

# Source includes the MIG header under a fixed name "gpu_server.h"; the
# user_h filename above is unique to avoid clashing with other consumers
# of gpu_server.defs (bootstrap, gpu_server itself).  Provide a forwarding
# header so the .c can do `#include "gpu_server.h"` regardless.
set(GPU_FWD_H ${CMAKE_CURRENT_BINARY_DIR}/gpu_server.h)
add_custom_command(
  OUTPUT ${GPU_FWD_H}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GPU_USER_H} ${GPU_FWD_H}
  DEPENDS ${GPU_USER_H}
  COMMENT "Forwarding header: libgpu_console/gpu_server.h"
)

add_library(libgpu_console STATIC
  gpu_console.c
  ${GPU_USER_C}
  ${GPU_FWD_H}
)

add_dependencies(libgpu_console mig_mach_public_headers)

target_compile_options(libgpu_console PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686>
)

target_compile_definitions(libgpu_console PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(libgpu_console PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}        # gpu_server.h forwarding
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libcap
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libgpu_console PROPERTIES
  OUTPUT_NAME "gpu_console"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
