cmake_minimum_required(VERSION 3.16)
project(libxmm C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")
add_library(libxmm STATIC ${LIB_SOURCES})
# Ensure stdint is available for legacy headers
target_compile_options(libxmm PRIVATE -include stdint.h)

target_include_directories(libxmm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

# Generazione automatica dei file MIG (.c e .h) a partire dai .defs nella cartella
file(GLOB MIG_DEFS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.defs")
foreach(DEFS IN LISTS MIG_DEFS)
  add_mig_defs(${DEFS} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/mach_kernel ${CMAKE_SOURCE_DIR}/export/powermac/include)
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()


set_target_properties(libxmm PROPERTIES
  OUTPUT_NAME "xmm"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${CMAKE_SYSTEM_NAME}/lib"
)
