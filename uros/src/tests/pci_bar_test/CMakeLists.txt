# pci_bar_test — the BAR decode, judged on the build machine (#427).
#
# 🔑 A HOST test, and that is the design rather than a shortcut.  The decode is
# a pure function from six numbers to a list of regions, and the defect it
# guards against needs a device placed above four gigabytes to appear on real
# hardware -- which no machine here does unless told to.  A check that waited
# for one would be a check that never ran.
#
# So it opts out of the tree-wide -ffreestanding exactly as migcom does, and
# for the same reason: it runs on the machine doing the build and uses that
# machine's C library.  See src/lib/migcom/CMakeLists.txt.
get_directory_property(_uros_dir_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _uros_dir_opts -ffreestanding)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_uros_dir_opts}")

# Built twice, once per ABI.
#
# ⚠️ The only thing about this code that could differ between the targets is
# the ABI it is compiled for: there are no pointers and no `long' in the
# computation, so the arithmetic is uint32_t in and uint64_t out, which are the
# same two types on both.  Building it both ways is what turns that from a
# claim into a result -- and it is why the arch flag is passed here while the
# rest of the file is host-shaped.
foreach(abi 32 64)
  add_executable(pci_bar_test_m${abi}
    pci_bar_test.c
    ${CMAKE_SOURCE_DIR}/src/lib/libpci/pci_bar.c
  )

  target_compile_options(pci_bar_test_m${abi} PRIVATE -std=gnu11 -m${abi})
  target_link_options(pci_bar_test_m${abi} PRIVATE -m${abi})

  target_include_directories(pci_bar_test_m${abi} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib/libpci
    ${CMAKE_SOURCE_DIR}/uapi
  )

  set_target_properties(pci_bar_test_m${abi} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
  )

  add_test(NAME pci_bar_decode_m${abi}
           COMMAND $<TARGET_FILE:pci_bar_test_m${abi}>)
endforeach()
