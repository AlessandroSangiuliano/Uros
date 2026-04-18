cmake_minimum_required(VERSION 3.16)
project(libflipc C)

# Source files
set(LIB_SOURCES
  flipc_domain.c
  flipc_alloc.c
  flipc_endpoint.c
  flipc_epgroup.c
  flipc_buffer.c
)

add_library(libflipc STATIC ${LIB_SOURCES})

# Depend on MIG-generated device/device.h (defined in parent CMakeLists)
add_dependencies(libflipc mig_device_header mig_mach_public_headers)

target_compile_options(libflipc PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -m32 -march=i686>
)

target_include_directories(libflipc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libflipc PROPERTIES
  OUTPUT_NAME "flipc"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
