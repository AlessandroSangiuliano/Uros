# mkbundle — host tool that builds the stage-1 multiboot bundle (Issue #186).

# Opt out of the tree-wide -ffreestanding (#506): this runs on the build
# machine and uses that machine's C library.  See src/lib/migcom for the note.
get_directory_property(_uros_dir_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _uros_dir_opts -ffreestanding)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_uros_dir_opts}")

add_executable(mkbundle mkbundle.c)

target_include_directories(mkbundle PRIVATE
  ${CMAKE_SOURCE_DIR}/src/servers/bootstrap
)

set_target_properties(mkbundle PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
