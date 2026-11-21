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
project(hello_pie C)

# A position-independent program that names no dynamic linker, for exec_server
# to relocate (#963).  Linked as block_device_server is -- -pie and
# --no-dynamic-linker, so an ET_DYN with a RELA table and no PT_INTERP -- which
# is the shape chora has, with no library under it, so that what it checks is
# exec_server's relocation and nothing else.
add_executable(hello_pie_bin
  hello_pie.c
)

target_compile_options(hello_pie_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS}
  -fPIE
  -fno-builtin
  -fno-stack-protector       # no canary -- there is no library to set one up
  -ffreestanding
)

target_link_options(hello_pie_bin PRIVATE
  ${UROS_ARCH_FLAGS} -pie -nostdlib
  -Wl,--no-dynamic-linker
  -Wl,-e,_start
  -Wl,--build-id=none
)

set_target_properties(hello_pie_bin PROPERTIES
  OUTPUT_NAME "hello_pie"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
