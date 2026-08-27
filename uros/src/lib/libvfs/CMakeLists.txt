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
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_CURRENT_SOURCE_DIR}
    -header ${VFS_USER_H}
    -sheader ${VFS_SERVER_H}
    -user ${VFS_USER_C}
    -server /dev/null
    ${VFS_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${VFS_DEFS} ${VFS_TYPES}
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
  
  -include stdint.h
)

target_include_directories(libvfs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}                    # vfs_types.h (public API)
  ${CMAKE_CURRENT_BINARY_DIR}                    # vfs.h (MIG user header)
  ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
  ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, mach.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach  # sys/timers.h
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${UROS_LIBPTHREADS_MD}
  ${UROS_ARCH_INCLUDE}
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
  # 🔥 UROS_BUILD_MUSL, not `if(TARGET musl_extproj)' (#434).
  #
  # This target is created from add_subdirectory(src), which runs BEFORE
  # add_subdirectory(src/contrib) -- so when the old guard was evaluated
  # musl_extproj did not exist yet, the condition was false, and the
  # dependency was never added.  Silently: a guard written to guarantee an
  # ordering was the reason there was none.
  #
  # It is not that add_dependencies() needs the target to exist; a dozen
  # other call sites in src/ name musl_extproj with no guard at all and are
  # ordered correctly, because CMake resolves the name at generate time.
  # The option is known at configure time and does not depend on directory
  # order, so it is what the condition asks about.
  if(UROS_BUILD_MUSL)
    add_dependencies(libvfs-pic musl_extproj)
  endif()
  set_target_properties(libvfs-pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(libvfs-pic PRIVATE
    -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -ffreestanding
    -fno-stack-protector  -include stdint.h
  )
  target_include_directories(libvfs-pic PRIVATE
    ${_MUSL_INC}                                   # musl headers FIRST
    ${CMAKE_CURRENT_SOURCE_DIR}                    # vfs_types.h, libvfs_internal.h
    ${CMAKE_CURRENT_BINARY_DIR}                    # vfs.h (MIG user header)
    ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
    ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, mach.h
    ${UROS_ARCH_INCLUDE}
    ${CMAKE_SOURCE_DIR}/src/mach_kernel
    ${CMAKE_SOURCE_DIR}/include
  )
  set_target_properties(libvfs-pic PROPERTIES OUTPUT_NAME "vfs-pic")
endif()

# #434: <pthread.h> pulls <machine/pthread_impl.h>, which libpthreads
# PUBLISHES into generated/include.  This target uses the header without
# linking the library, so nothing ordered it before the copy and a build
# from an empty directory could compile first and fail.
if(TARGET libvfs)
  add_dependencies(libvfs pthread_impl_published)
endif()
if(TARGET libvfs-pic)
  add_dependencies(libvfs-pic pthread_impl_published)
endif()
