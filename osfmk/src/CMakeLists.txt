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


