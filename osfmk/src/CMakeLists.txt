cmake_minimum_required(VERSION 3.16)

# Questa directory contiene l'intero albero dei sorgenti Mach.

set(OSFMK_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Per ora costruiamo solo i tool host necessari (mig/migcom, librerie base).

if(OSFMK_BUILD_TOOLS)
  add_subdirectory(mach_services/lib)
endif()

# Kernel build (requires migcom to be built first)
if(OSFMK_BUILD_KERNEL)
  add_subdirectory(mach_kernel)
endif()

# Build bootstrap server
if(OSFMK_BUILD_BOOTSTRAP)
  add_subdirectory(bootstrap)
endif()
#
# if(OSFMK_BUILD_DEFAULT_PAGER)
#   add_subdirectory(default_pager)
# endif()


