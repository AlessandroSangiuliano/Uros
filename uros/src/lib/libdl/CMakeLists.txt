cmake_minimum_required(VERSION 3.16)
project(libdl C)

set(LIB_SOURCES
  dl_api.c
  elf_load.c
  elf_reloc.c
  elf_symbol.c
  dl_self.c
)

# #423: -DUROS_ABLATE_423_RELTAG=ON makes libdl look for the OTHER machine's
# relocation tag, so the table is never found and nothing is relocated.  Off in
# every build.  It exists because dl_test's four arms passed at the first
# attempt, and arms that have never been red are a claim about the test rather
# than about the loader.
#
# 🔑 The second ablation tried.  The first swapped the SOURCE of the addend --
# r_addend against the slot -- and changed nothing: GNU ld writes the addend
# into both, so the two readings agree on every entry dl_module.so has.  A
# negative result, and the reason the comment in elf_reloc.c no longer claims
# that reading the wrong one is observably wrong.
option(UROS_ABLATE_423_RELTAG "Verify dl_test by looking for the wrong relocation tag" OFF)

add_library(libdl STATIC ${LIB_SOURCES})

if(UROS_ABLATE_423_RELTAG)
  target_compile_definitions(libdl PRIVATE ABLATE_423_RELTAG=1)
endif()

add_dependencies(libdl mig_mach_public_headers)

target_compile_options(libdl PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin  -mstack-protector-guard=global -include stdint.h>
)

target_include_directories(libdl PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# #423: libdl reads the file image through libelf instead of parsing it a
# second time.  PUBLIC on the link, because a static library's dependency has
# to reach whoever links libdl -- the four servers that load driver modules do
# not name libelf themselves.
target_link_libraries(libdl PUBLIC libelf)

target_include_directories(libdl PRIVATE
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libelf
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libdl PROPERTIES
  OUTPUT_NAME "dl"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
