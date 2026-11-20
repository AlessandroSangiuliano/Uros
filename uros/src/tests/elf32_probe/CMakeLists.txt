# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

cmake_minimum_required(VERSION 3.16)
project(elf32_probe C)

# An i386 image made by an x86-64 build, for exec_server to refuse (#963).
#
# The compiler driver with -m32 assembles with `as --32' and links with
# `ld -m elf_i386', which binutils for x86-64 carry.  With -nostdlib it asks
# for no 32-bit library and no 32-bit start file -- which is the part a host
# without 32-bit packages does not have, and why this is not add_executable:
# the project's flags for this target would make it an x86-64 program.
set(ELF32_PROBE_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin/elf32_probe")

add_custom_command(
  OUTPUT ${ELF32_PROBE_OUT}
  COMMAND ${CMAKE_C_COMPILER} -m32 -nostdlib -static
          -Wl,-e,_start -Wl,--build-id=none
          -o ${ELF32_PROBE_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/elf32_probe.S
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/elf32_probe.S
  COMMENT "elf32_probe: an i386 image for exec_server to refuse (#963)"
  VERBATIM
)
add_custom_target(elf32_probe ALL DEPENDS ${ELF32_PROBE_OUT})
