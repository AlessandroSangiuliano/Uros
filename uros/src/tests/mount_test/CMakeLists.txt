cmake_minimum_required(VERSION 3.16)
project(mount_test C)

# ===========================================================================
# Who may mount where, and what a reader takes from a mount's server (#716)
# ===========================================================================
#
# x86-64 only, as netname_test: the name server and libvfs are common code, and
# i386's suite boots ext_server and proc_server under the same rules, which is
# what it is there for.
#
# ⚠️ ONE TASK, BOTH SIDES OF vfs.defs.  It reads through libvfs, as every
# program does, and it serves the mount it reads from, which is how a server
# that answers more than was asked for can be put in front of libvfs at all.
# The two sides cannot share their names: vfs.defs gives its server functions
# the prefix vfs_, which is also libvfs's API -- vfs_open, vfs_read and the
# rest are each defined twice.  So the server stubs generated here are compiled
# with liar_names.h, which renames the sixteen functions they call, and the
# server's own file includes it too and never sees libvfs.h.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(VFS_DEFS  ${CMAKE_SOURCE_DIR}/src/lib/libvfs/vfs.defs)
set(VFS_TYPES ${CMAKE_SOURCE_DIR}/src/lib/libvfs/vfs_types.h)

set(VFS_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.c)
set(VFS_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/vfs_server.h)

# The server half only: the user stubs are libvfs's, linked below.
add_custom_command(
  OUTPUT ${VFS_SERVER_C} ${VFS_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    ${UROS_MIG_TARGET_ARGS}
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/lib/libvfs
    -header /dev/null
    -sheader ${VFS_SERVER_H}
    -user /dev/null
    -server ${VFS_SERVER_C}
    ${VFS_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${VFS_DEFS} ${VFS_TYPES}
  COMMENT "MIG: vfs.defs (server stubs for mount_test's own mount)"
  VERBATIM
)

# #574: the server half of notify.defs, for [25], which hands a port-destroyed
# notification the kernel sent to the stub MIG generates for it -- the stub
# bootstrap's refused every one of on x86-64.
set(NOTIFY_SERVER_C ${CMAKE_CURRENT_BINARY_DIR}/notify_server.c)
set(NOTIFY_SERVER_H ${CMAKE_CURRENT_BINARY_DIR}/notify_server.h)
add_custom_command(
  OUTPUT ${NOTIFY_SERVER_C} ${NOTIFY_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    ${UROS_MIG_TARGET_ARGS}
    -I${UROS_UAPI_DIR}
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header /dev/null
    -sheader ${NOTIFY_SERVER_H}
    -user /dev/null
    -server ${NOTIFY_SERVER_C}
    ${UROS_UAPI_DIR}/mach/notify.defs
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${UROS_UAPI_DIR}/mach/notify.defs
  COMMENT "MIG: notify.defs (server stubs for mount_test's [25])"
  VERBATIM
)

add_executable(mount_test_bin mount_test.c mount_liar.c mount_notify.c
               ${VFS_SERVER_C} ${NOTIFY_SERVER_C})

set_source_files_properties(${VFS_SERVER_C} PROPERTIES
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/liar_names.h")

add_dependencies(mount_test_bin mig_mach_public_headers libvfs libflipc2
                 libcap libnetname libmach libpthreads)

target_compile_options(mount_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(mount_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(mount_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}                    # vfs_server.h
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/src/lib/libvfs
  ${CMAKE_BINARY_DIR}/src/lib/libvfs             # vfs.h: fs_open() by hand, #911
  ${CMAKE_BINARY_DIR}/src/lib/libflipc2          # flipc2_endpoint.h, #911
)

# libvfs exports the rest of its include directories, which are this
# program's too; the group because libpthreads and libmach reach into each
# other and the libraries above them into both.
target_link_libraries(mount_test_bin PRIVATE
  -Wl,--start-group
  libvfs
  libflipc2
  libcap
  libnetname
  libpthreads
  libmach
  -Wl,--end-group
)

target_link_options(mount_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(mount_test_bin PROPERTIES
  OUTPUT_NAME "mount_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)

# Its policy: a mount point, and only its own.
set(MOUNT_TEST_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/mount_test.cmf")
add_custom_command(
  OUTPUT ${MOUNT_TEST_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${MOUNT_TEST_MANIFEST_OUT}
          ${CMAKE_CURRENT_SOURCE_DIR}/mount_test.manifest
  DEPENDS mkmanifest ${CMAKE_CURRENT_SOURCE_DIR}/mount_test.manifest
  COMMENT "mkmanifest: mount_test.manifest -> mount_test.cmf"
  VERBATIM
)
add_custom_target(mount_test_manifest ALL DEPENDS ${MOUNT_TEST_MANIFEST_OUT})
add_dependencies(mount_test_bin mount_test_manifest)
