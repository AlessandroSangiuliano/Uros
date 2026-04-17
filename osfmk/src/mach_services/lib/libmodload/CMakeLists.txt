cmake_minimum_required(VERSION 3.16)
project(libmodload C)

set(LIB_SOURCES
  modload.c
)

add_library(libmodload STATIC ${LIB_SOURCES})

target_compile_options(libmodload PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686>
)

target_include_directories(libmodload PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(libmodload PRIVATE
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libdl
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libmodload PROPERTIES
  OUTPUT_NAME "modload"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
