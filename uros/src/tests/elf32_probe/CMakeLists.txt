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

# Three images for exec_server to refuse (#963), from one source: an i386
# program, an i386 shared object to be named as a dynamic linker, and an
# x86-64 program that names it.
#
# The compiler driver with -m32 assembles with `as --32' and links with
# `ld -m elf_i386', which binutils for x86-64 carry.  With -nostdlib it asks
# for no 32-bit library and no 32-bit start file -- which is the part a host
# without 32-bit packages does not have, and why these are not
# add_executable: the project's flags for this target would make them x86-64.
set(_E32_SBIN "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")
set(_E32_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/elf32_probe.S")

add_custom_command(
  OUTPUT ${_E32_SBIN}/elf32_probe
  COMMAND ${CMAKE_C_COMPILER} -m32 -nostdlib -static
          -Wl,-e,_start -Wl,--build-id=none
          -o ${_E32_SBIN}/elf32_probe ${_E32_SRC}
  DEPENDS ${_E32_SRC}
  COMMENT "elf32_probe: an i386 program for exec_server to refuse (#963)"
  VERBATIM
)
add_custom_command(
  OUTPUT ${_E32_SBIN}/elf32_ldso_probe
  COMMAND ${CMAKE_C_COMPILER} -m32 -nostdlib -shared
          -Wl,-e,_start -Wl,--build-id=none
          -o ${_E32_SBIN}/elf32_ldso_probe ${_E32_SRC}
  DEPENDS ${_E32_SRC}
  COMMENT "elf32_ldso_probe: an i386 dynamic linker for exec_server to refuse (#963)"
  VERBATIM
)
# -pie with -dynamic-linker gives a PT_INTERP and no DT_NEEDED: what is asked
# of exec_server is the dynamic linker, and nothing else.
add_custom_command(
  OUTPUT ${_E32_SBIN}/interp_probe
  COMMAND ${CMAKE_C_COMPILER} -nostdlib -pie
          -Wl,-dynamic-linker,/elf32_ldso_probe
          -Wl,-e,_start -Wl,--build-id=none
          -o ${_E32_SBIN}/interp_probe ${_E32_SRC}
  DEPENDS ${_E32_SRC}
  COMMENT "interp_probe: an x86-64 program whose dynamic linker is i386 (#963)"
  VERBATIM
)
add_custom_target(elf32_probe ALL
  DEPENDS ${_E32_SBIN}/elf32_probe ${_E32_SBIN}/elf32_ldso_probe
          ${_E32_SBIN}/interp_probe)
