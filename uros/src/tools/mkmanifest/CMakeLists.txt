# mkmanifest — host tool that compiles a plain-text manifest source
# into the binary .cmf TLV blob consumed by cap_server (#216 v2.1).

add_executable(mkmanifest mkmanifest.c)

target_include_directories(mkmanifest PRIVATE
  ${CMAKE_SOURCE_DIR}/uapi
)

set_target_properties(mkmanifest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
