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
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_include_directories(libelf PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}                    # libelf.h (public API)
  ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
  ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, string.h
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel             # mach/elf.h fallback
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libelf PROPERTIES
  OUTPUT_NAME "elf"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
