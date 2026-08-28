# mkmanifest — host tool that compiles a plain-text manifest source
# into the binary .cmf TLV blob consumed by cap_server (#216 v2.1).

# Opt out of the tree-wide -ffreestanding (#506): this runs on the build
# machine and uses that machine's C library.  See src/lib/migcom for the note.
get_directory_property(_uros_dir_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _uros_dir_opts -ffreestanding)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_uros_dir_opts}")

add_executable(mkmanifest mkmanifest.c)

target_include_directories(mkmanifest PRIVATE
  ${CMAKE_SOURCE_DIR}/uapi
)

set_target_properties(mkmanifest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
