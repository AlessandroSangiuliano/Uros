cmake_minimum_required(VERSION 3.16)
project(libblk C)

add_library(libblk STATIC blk.c)

add_dependencies(libblk mig_device_header)

target_compile_options(libblk PRIVATE
  -std=gnu11 -fno-builtin -w -mstack-protector-guard=global
  -include stdint.h -m32 -march=i686 -msse -msse2
)

target_compile_definitions(libblk PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(libblk PUBLIC
  # blk.h header
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(libblk PRIVATE
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libblk PROPERTIES
  OUTPUT_NAME "blk"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
