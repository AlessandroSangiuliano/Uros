cmake_minimum_required(VERSION 3.16)
project(hello_exec C)

add_executable(hello_exec_bin
  hello_exec.c
)

target_compile_options(hello_exec_bin PRIVATE
  -std=gnu11
  -m32 -march=i686
  -fno-builtin
  -fno-stack-protector       # no canary — we have no libc to set one up
  -ffreestanding
  -include stdint.h
)

# Tiny static binary, no libc, fixed entry _start.  Use ET_EXEC
# (not PIE) since exec_server v0.1.0 only handles static images.
target_link_options(hello_exec_bin PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,_start
  -Wl,--build-id=none
  -no-pie
  # Place text at the conventional 1 MiB mark used by Uros server
  # binaries so the chosen EXEC_STACK_VA (0xBF800000) sits well above
  # any plausible PT_LOAD.
  -Wl,-Ttext-segment=0x100000
)

set_target_properties(hello_exec_bin PROPERTIES
  OUTPUT_NAME "hello_exec"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
