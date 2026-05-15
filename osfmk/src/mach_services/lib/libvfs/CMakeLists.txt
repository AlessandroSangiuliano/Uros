cmake_minimum_required(VERSION 3.16)
project(libvfs C)

# ===========================================================================
# MIG-generated vfs user stubs (subsystem 3000, #220 sub-step C)
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(VFS_DEFS  ${CMAKE_CURRENT_SOURCE_DIR}/vfs.defs)
set(VFS_TYPES ${CMAKE_CURRENT_SOURCE_DIR}/vfs_types.h)

set(VFS_USER_C   ${CMAKE_CURRENT_BINARY_DIR}/vfs_user.c)
set(VFS_USER_H   ${CMAKE_CURRENT_BINARY_DIR}/vfs.h)
set(VFS_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.h)

add_custom_command(
  OUTPUT ${VFS_USER_C} ${VFS_USER_H} ${VFS_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${VFS_USER_H}
    -sheader ${VFS_SERVER_H}
    -user ${VFS_USER_C}
    -server /dev/null
    ${VFS_DEFS}
  DEPENDS migcom ${VFS_DEFS} ${VFS_TYPES}
  COMMENT "MIG: vfs.defs (libvfs user stubs)"
  VERBATIM
)

add_library(libvfs STATIC
  libvfs.c
  ${VFS_USER_C}
)

add_dependencies(libvfs mig_mach_public_headers)

target_compile_options(libvfs PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_include_directories(libvfs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}                    # vfs_types.h (public API)
  ${CMAKE_CURRENT_BINARY_DIR}                    # vfs.h (MIG user header)
  ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
  ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, mach.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach  # sys/timers.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  ${CMAKE_SOURCE_DIR}/export/include             # mach/, servers/netname.h
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libvfs PROPERTIES
  OUTPUT_NAME "vfs"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
