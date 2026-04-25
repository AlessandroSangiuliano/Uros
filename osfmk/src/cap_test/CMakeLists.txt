cmake_minimum_required(VERSION 3.16)
project(cap_test C)

add_executable(cap_test_server
  cap_test.c
)

add_dependencies(cap_test_server mig_mach_public_headers)

target_compile_options(cap_test_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(cap_test_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(cap_test_server PRIVATE
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libcap
)

target_link_libraries(cap_test_server PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libcap
  libmach
  libsa_mach
  -Wl,--end-group
)

target_link_options(cap_test_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(cap_test_server PROPERTIES
  OUTPUT_NAME "cap_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
