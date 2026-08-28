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
  
  -include stdint.h
)

target_include_directories(libelf PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}                    # libelf.h (public API)
  ${CMAKE_BINARY_DIR}/generated/include          # mach/machine -> i386
  ${CMAKE_SOURCE_DIR}/src/mach_services/include  # sa_mach.h, mach.h
  # ⚠️ <string.h> is in sa_mach/, not in the directory above it, so the line
  # above never supplied it and the comment that said so was wrong: libelf's
  # memcmp and strcmp were declared by the build machine's glibc (#506).
  # Same class as #495, where libcap and cap_server ended up compiled against
  # two different C libraries and agreed only by luck.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel             # mach/elf.h fallback
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libelf PROPERTIES
  OUTPUT_NAME "elf"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
