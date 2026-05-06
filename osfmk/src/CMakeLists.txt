cmake_minimum_required(VERSION 3.16)

# Questa directory contiene l'intero albero dei sorgenti Mach.

set(OSFMK_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# The ODE export directory uses pseudo-symlinks ("link ppc") for mach/machine.
# We target i386, so create a real symlink: mach/machine -> mach/i386 headers.
# This is shared by all subprojects via ${CMAKE_BINARY_DIR}/generated/include.
set(GEN_MACH_MACHINE "${CMAKE_BINARY_DIR}/generated/include/mach/machine")
if(NOT EXISTS "${GEN_MACH_MACHINE}")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/mach")
  file(CREATE_LINK
    "${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/i386"
    "${GEN_MACH_MACHINE}"
    SYMBOLIC)
endif()

# Per ora costruiamo solo i tool host necessari (mig/migcom, librerie base).

if(OSFMK_BUILD_TOOLS)
  add_subdirectory(mach_services/lib)
  add_subdirectory(file_systems)
  add_subdirectory(tools/mkbundle)
endif()

# Kernel build (requires migcom to be built first)
if(OSFMK_BUILD_KERNEL)
  add_subdirectory(mach_kernel)
endif()

# Build bootstrap server
if(OSFMK_BUILD_BOOTSTRAP)
  add_subdirectory(bootstrap)
endif()

# Build default pager server
if(OSFMK_BUILD_DEFAULT_PAGER)
  add_subdirectory(default_pager)
endif()

# Build hello server (test/demo server)
if(OSFMK_BUILD_HELLO_SERVER)
  add_subdirectory(hello_server)
endif()

# Build IPC performance benchmark
if(OSFMK_BUILD_IPC_BENCH)
  add_subdirectory(ipc_bench)
endif()

# Build AHCI userspace driver
if(OSFMK_BUILD_AHCI_DRIVER)
  add_subdirectory(ahci_driver)
endif()

# Build virtio-blk userspace driver
if(OSFMK_BUILD_VIRTIO_BLK)
  add_subdirectory(virtio_blk)
endif()

# Build modular block device server
if(OSFMK_BUILD_BLOCK_SERVER)
  add_subdirectory(block_device_server)
endif()

# Build HAL server (hardware discovery + device registry)
if(OSFMK_BUILD_HAL_SERVER)
  add_subdirectory(hal_server)
endif()

# Build name server (netname)
if(OSFMK_BUILD_NAME_SERVER)
  add_subdirectory(mach_services/servers/netname)
endif()

# Build ext filesystem server (ext2/ext3/ext4)
if(OSFMK_BUILD_EXT2_SERVER)
  add_subdirectory(ext_server)
endif()

# Build pthread test server
if(OSFMK_BUILD_PTHREAD_TEST)
  add_subdirectory(pthread_test)
endif()

# Build capability server (Uros cap_server)
if(OSFMK_BUILD_CAP_SERVER)
  add_subdirectory(cap_server)
endif()

# Build capability negative-test binary
if(OSFMK_BUILD_CAP_TEST)
  add_subdirectory(cap_test)
endif()

# Build gpu_server (#194 — userspace GPU/display server)
if(OSFMK_BUILD_GPU_SERVER)
  add_subdirectory(gpu_server)
endif()
