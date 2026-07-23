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
  vfs_flipc.c          # FLIPC v2 fast-path (#232); split out in #234.
                       # The umbrella libc.so links vfs_flipc_stub.c instead.
  ${VFS_USER_C}
)

add_dependencies(libvfs mig_mach_public_headers)

target_compile_options(libvfs PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
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
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/AT386
  ${CMAKE_SOURCE_DIR}/export/include             # mach/, servers/netname.h
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libvfs PROPERTIES
  OUTPUT_NAME "vfs"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)

# ---------------------------------------------------------------------------
# PIC variant for the umbrella libc.so (#234 incr 2b).
#
# Two deliberate differences from the static libvfs:
#   1. Linked with vfs_flipc_stub.c instead of vfs_flipc.c — no FLIPC code
#      (or libflipc2 dependency) leaks into libc.so; the dynamic linker and
#      every dynamic app read via plain Mach RPC.
#   2. Compiled against the MUSL headers (not the custom libpthreads/sa_mach
#      ones) so pthread_mutex_t et al. match musl's ABI — libc.so's own
#      pthread_mutex_lock operates on these objects.  The static libvfs keeps
#      the custom headers until the static servers migrate off libpthreads.
# ---------------------------------------------------------------------------
if(UROS_BUILD_MUSL)
  set(_MUSL_INC ${CMAKE_BINARY_DIR}/src/contrib/musl-install/include)
  add_library(libvfs-pic STATIC
    libvfs.c
    vfs_flipc_stub.c
    ${VFS_USER_C}
  )
  add_dependencies(libvfs-pic mig_mach_public_headers)
  if(TARGET musl_extproj)
    add_dependencies(libvfs-pic musl_extproj)
  endif()
  set_target_properties(libvfs-pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(libvfs-pic PRIVATE
    -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -ffreestanding
    -fno-stack-protector -w -include stdint.h
  )
  target_include_directories(libvfs-pic PRIVATE
    ${_MUSL_INC}                                   # musl headers FIRST
    ${CMAKE_CURRENT_SOURCE_DIR}                    # vfs_types.h, libvfs_internal.h
    ${CMAKE_CURRENT_BINARY_DIR}                    # vfs.h (MIG user header)
    ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
    ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, mach.h
    ${CMAKE_SOURCE_DIR}/export/include             # mach/, servers/netname.h
    ${CMAKE_SOURCE_DIR}/src/mach_kernel
    ${CMAKE_SOURCE_DIR}/include
  )
  set_target_properties(libvfs-pic PROPERTIES OUTPUT_NAME "vfs-pic")
endif()
