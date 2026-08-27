cmake_minimum_required(VERSION 3.16)
project(libdl C)

set(LIB_SOURCES
  dl_api.c
  elf_load.c
  elf_reloc.c
  elf_symbol.c
  dl_self.c
)

add_library(libdl STATIC ${LIB_SOURCES})

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
