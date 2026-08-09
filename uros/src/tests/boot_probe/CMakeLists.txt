cmake_minimum_required(VERSION 3.16)
project(boot_probe C ASM)

# boot_probe (#422) — the first 64-bit image the kernel loads.
#
# Built outside the userland gate in src/CMakeLists.txt on purpose: everything
# behind that gate needs musl (#414) or libmach (#426), and this needs neither.
# It is a page of assembly and a stack.  See boot_probe.S for what it proves
# and, more importantly, what it does not.
#
# x86-64 only.  On i386 the equivalent already exists many times over — every
# server in the bundle is one — and the entry sequence would be a different
# one anyway: i386 reaches the kernel through a Mach trap, not SYSCALL.
if(NOT UROS_TARGET_ARCH STREQUAL "x86_64")
  return()
endif()

add_executable(boot_probe boot_probe.S)

# The Mach trap stubs (#426), and the only thing this image links.
#
# ⚠️ It used to link nothing at all, and the comment above still says the
# gate does not apply because it needs neither musl nor libmach.  Half of
# that is now out of date on purpose: libmach's TRAP STUBS are exactly what
# an image with no libc can use, which is why they are built ahead of the
# gate too.  What it still does not need is a C library, a startup, or a
# heap — the stubs are three instructions each and reference nothing.
target_link_libraries(boot_probe PRIVATE libmach_traps)

# ⚠️ -nostdlib AND -static AND no entry symbol from a runtime: this must be an
# ET_EXEC with its own _start and nothing linked in behind it.  A dynamic image
# would need an interpreter (#423), and a runtime would need a libc.
#
# The load address is stated rather than left to the linker's default, which
# for x86-64 is 0x400000 and would work — but the number ends up in the ELF
# program headers, is what the kernel maps, and is exactly the sort of thing
# that should be visible in the file that decides it rather than inherited.
target_link_options(boot_probe PRIVATE
  -m64 -nostdlib -nostartfiles -static
  -Wl,-e,_start
  -Wl,--build-id=none
  -Wl,-Ttext-segment=0x400000
)

target_compile_options(boot_probe PRIVATE -m64)

set_target_properties(boot_probe PROPERTIES OUTPUT_NAME boot_probe)

# Into the same place the i386 build puts its user binaries, so the scripts
# that assemble a boot image find it by the same path on both targets.
add_custom_command(TARGET boot_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          ${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:boot_probe>
          ${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin/boot_probe
  COMMENT "boot_probe -> export/uros/${UROS_TARGET_ARCH}/user/sbin"
)
