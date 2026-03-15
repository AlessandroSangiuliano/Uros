cmake_minimum_required(VERSION 3.16)
project(libsa_fs C)

# ===========================================================================
# Source files
# ===========================================================================

# Core filesystem abstraction
set(SA_FS_SOURCES
  file_system.c
  page_cache.c
)

# i386 (AT386) filesystem dispatcher: UFS, EXT2FS, MINIXFS
list(APPEND SA_FS_SOURCES
  AT386/fs_switch.c
)

# UFS (BSD Unix File System)
# NOTE: ufs/strfcns.c excluded — strbuild() is only used by the bootstrap
# server, which has its own copy.
list(APPEND SA_FS_SOURCES
  ufs/ufs.c
)

# EXT2FS (Linux ext2)
list(APPEND SA_FS_SOURCES
  ext2fs/ext2fs.c
  ext2fs/ext2_ffs_compat.c
)

# MINIXFS
list(APPEND SA_FS_SOURCES
  minixfs/minixfs.c
  minixfs/minix_ffs_compat.c
)

# ===========================================================================
# Build
# ===========================================================================

add_library(libsa_fs STATIC ${SA_FS_SOURCES})

# ufs.c and ext2fs.c include <device/device.h> which is MIG-generated
add_dependencies(libsa_fs mig_device_header)

target_compile_options(libsa_fs PRIVATE
  -std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686 -msse -msse2
)

target_compile_definitions(libsa_fs PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(libsa_fs PUBLIC
  # Library headers (file_system.h, externs.h)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Filesystem-specific headers (each fs includes its own subdir)
  ${CMAKE_CURRENT_SOURCE_DIR}/ufs
  ${CMAKE_CURRENT_SOURCE_DIR}/ext2fs
  ${CMAKE_CURRENT_SOURCE_DIR}/minixfs
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services (mach.h, sa_mach.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # sa_mach standalone types (<types.h> -> sa_mach/types.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  # Kernel headers (device/, mach/, mach_debug/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Export headers (mach/mach_interface.h, mach/boot_info.h)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libsa_fs PROPERTIES
  OUTPUT_NAME "sa_fs"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
