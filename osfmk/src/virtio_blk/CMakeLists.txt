cmake_minimum_required(VERSION 3.16)
project(virtio_blk C)

# ===========================================================================
# MIG-generated device_master user stubs
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(DEVICE_MASTER_DEFS ${CMAKE_SOURCE_DIR}/src/mach_kernel/device/device_master.defs)

set(DM_USER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_user.c)
set(DM_USER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master.h)
set(DM_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/device_master_server.c)
set(DM_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/device_master_server.h)

add_custom_command(
  OUTPUT ${DM_USER_C} ${DM_USER_H} ${DM_SERVER_C} ${DM_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DM_USER_H}
    -sheader ${DM_SERVER_H}
    -user ${DM_USER_C}
    -server ${DM_SERVER_C}
    ${DEVICE_MASTER_DEFS}
  DEPENDS migcom ${DEVICE_MASTER_DEFS}
  COMMENT "MIG: device_master.defs (user stubs for virtio_blk)"
  VERBATIM
)

# ===========================================================================
# Source files
# ===========================================================================

set(VIRTIO_SOURCES
  virtio_blk.c
  nostdlib_stubs.c
  ${DM_USER_C}
  ${CMAKE_BINARY_DIR}/generated/device_server.c
)

set_source_files_properties(
  ${CMAKE_BINARY_DIR}/generated/device_server.c
  PROPERTIES GENERATED TRUE
)

# Copy device_server.h locally
set(LOCAL_DEVICE_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/device_server.h)
add_custom_command(
  OUTPUT ${LOCAL_DEVICE_SERVER_H}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/generated/device_server.h
    ${LOCAL_DEVICE_SERVER_H}
  DEPENDS ${CMAKE_BINARY_DIR}/generated/device_server.h
  COMMENT "Copy device_server.h for virtio_blk"
)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(virtio_blk_server
  ${VIRTIO_SOURCES}
  ${LOCAL_DEVICE_SERVER_H}
)

target_compile_options(virtio_blk_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(virtio_blk_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(virtio_blk_server PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(virtio_blk_server PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_mach
  libnetname
  -Wl,--end-group
)

target_link_options(virtio_blk_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

add_dependencies(virtio_blk_server mig_device_header mig_mach_public_headers)

set_target_properties(virtio_blk_server PROPERTIES
  OUTPUT_NAME "virtio_blk"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
