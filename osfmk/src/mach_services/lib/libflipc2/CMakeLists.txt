cmake_minimum_required(VERSION 3.16)
project(libflipc2 C)

set(LIB_SOURCES
  flipc2_channel.c
)

add_library(libflipc2 STATIC ${LIB_SOURCES})

add_dependencies(libflipc2 mig_device_header)

target_compile_options(libflipc2 PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -m32 -march=i686 -mstack-protector-guard=global>
)

target_include_directories(libflipc2 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libflipc2 PROPERTIES
  OUTPUT_NAME "flipc2"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
