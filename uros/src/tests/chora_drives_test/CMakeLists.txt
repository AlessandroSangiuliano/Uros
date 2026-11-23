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

# chora_drives_test -- the reading of a module's declaration, on the build
# machine (#964).
#
# A HOST test, as pci_bar_test is: chora_drives.c is pure, and the declarations
# a boot cannot show -- two modules naming one device alike, a line the HAL
# must refuse -- are lines this test writes.  It opts out of the tree-wide
# -ffreestanding as pci_bar_test does, and uses the build machine's C library.
get_directory_property(_uros_dir_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _uros_dir_opts -ffreestanding)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_uros_dir_opts}")

# Built twice, once per ABI: a line is 16 bytes on every target, and the HAL
# reads lines a module of another build wrote.
foreach(abi 32 64)
  add_executable(chora_drives_test_m${abi}
    chora_drives_test.c
    ${CMAKE_SOURCE_DIR}/src/lib/libdriver/chora_drives.c
  )
  target_compile_options(chora_drives_test_m${abi} PRIVATE -std=gnu11 -m${abi})
  target_link_options(chora_drives_test_m${abi} PRIVATE -m${abi})
  target_include_directories(chora_drives_test_m${abi} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib/libdriver
  )
  set_target_properties(chora_drives_test_m${abi} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
  )
  add_test(NAME chora_drives_m${abi}
           COMMAND $<TARGET_FILE:chora_drives_test_m${abi}>)
endforeach()
