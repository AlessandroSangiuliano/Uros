cmake_minimum_required(VERSION 3.16)
project(msg_server_test C)

# A right a server kept must survive the server's next message (#583).
#
# A client of libmach's mach_msg_server and nothing else: the loop is what is
# under test, so the program runs it in a thread of its own and talks to it.
# Built on both targets from the same source -- the loop is the same code on
# both, and so was the defect.

add_executable(msg_server_test_bin msg_server_test.c)

add_dependencies(msg_server_test_bin mig_mach_public_headers)

target_compile_options(msg_server_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS}
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(msg_server_test_bin PRIVATE
  MACH_IPC_COMPAT=0
)

# --start-group because libpthreads and libmach reach into each other.
target_link_libraries(msg_server_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group
)

target_link_options(msg_server_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(msg_server_test_bin PROPERTIES
  OUTPUT_NAME "msg_server_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
