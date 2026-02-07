cmake_minimum_required(VERSION 3.16)
project(libservice C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")

# Generazione MIG per .defs (service.defs può trovarsi in include/servers o export/.../servers)
file(GLOB MIG_DEFS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.defs"
  "${CMAKE_SOURCE_DIR}/src/mach_services/include/servers/*.defs"
  "${CMAKE_SOURCE_DIR}/export/powermac/include/servers/*.defs"
)
foreach(DEFS IN LISTS MIG_DEFS)
  # Includi sia la cartella con le defs sia l'include export per risolvere #include <...>
  add_mig_defs(${DEFS} ${CMAKE_SOURCE_DIR}/src/mach_services/include/servers ${CMAKE_SOURCE_DIR}/export/powermac/include)
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()

add_library(libservice STATIC ${LIB_SOURCES})
# Ensure stdint is available for legacy headers
target_compile_options(libservice PRIVATE -include stdint.h)

target_include_directories(libservice PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libservice PROPERTIES
  OUTPUT_NAME "service"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
