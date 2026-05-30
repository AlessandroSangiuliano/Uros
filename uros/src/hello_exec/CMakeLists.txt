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

# ===========================================================================
# #235 — embedded .uros_manifest section.
#
# mkmanifest compiles the plain-text manifest into a binary .cmf
# blob, then objcopy --add-section appends it to the built binary as
# a non-loadable ELF section named .uros_manifest.  exec_server
# extracts the section at load time via libelf and hands it to
# cap_server through cap_provision.
# ===========================================================================

set(HELLO_EXEC_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/hello_exec.manifest)
set(HELLO_EXEC_CMF         ${CMAKE_CURRENT_BINARY_DIR}/hello_exec.cmf)

add_custom_command(TARGET hello_exec_bin POST_BUILD
  COMMAND $<TARGET_FILE:mkmanifest> -o ${HELLO_EXEC_CMF} ${HELLO_EXEC_MANIFEST_SRC}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${HELLO_EXEC_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            $<TARGET_FILE:hello_exec_bin>
  DEPENDS mkmanifest ${HELLO_EXEC_MANIFEST_SRC}
  COMMENT "embed .uros_manifest into hello_exec"
  VERBATIM
)
