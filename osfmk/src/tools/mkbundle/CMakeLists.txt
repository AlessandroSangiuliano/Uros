# mkbundle — host tool that builds the stage-1 multiboot bundle (Issue #186).

add_executable(mkbundle mkbundle.c)

target_include_directories(mkbundle PRIVATE
  ${CMAKE_SOURCE_DIR}/src/bootstrap
)

set_target_properties(mkbundle PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
