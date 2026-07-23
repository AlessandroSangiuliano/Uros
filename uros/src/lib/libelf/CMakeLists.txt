cmake_minimum_required(VERSION 3.16)
project(libelf C)

# ===========================================================================
# libelf — pure ELF parser (#227)
# Original Uros code, no derivation from third-party ELF parsers.
# ===========================================================================

add_library(libelf STATIC
  libelf.c
)

target_compile_options(libelf PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_include_directories(libelf PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}                    # libelf.h (public API)
  ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
  ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, string.h
  ${CMAKE_SOURCE_DIR}/export/include             # mach/elf.h
  ${CMAKE_SOURCE_DIR}/src/mach_kernel             # mach/elf.h fallback
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libelf PROPERTIES
  OUTPUT_NAME "elf"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
