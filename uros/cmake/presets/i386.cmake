# The i386 build the harness runs, as a CMake initial cache (#653).
#
#   cmake -G Ninja -C uros/cmake/presets/i386.cmake -S uros -B uros/build
#
# Only what differs from the defaults is here.  i386's defaults are the
# historical minimal ones -- one processor, no musl, and none of the servers
# or tests the bundle boots -- so this file is longer than x86-64's.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(UROS_TARGET_ARCH i386 CACHE STRING "")
set(UROS_NCPUS 64 CACHE STRING "")
set(UROS_BUILD_MUSL ON CACHE BOOL "")

# Release turns the kernel symbol table off by default; every build the harness
# has judged had it on.
set(OSFMK_GEN_KSYMS ON CACHE BOOL "")

# The servers, drivers and tests the bundle boots.
foreach(_part
    AHCI_DRIVER BLOCK_SERVER CAP_SERVER CAP_TEST CHAR_SERVER DEFAULT_PAGER
    EXEC_SERVER EXT2_SERVER GPU_SERVER GPUSTAT HAL_SERVER HELLO_EXEC
    HELLO_SERVER IPC_BENCH NAME_SERVER PROC_SERVER PTHREAD_TEST VIRTIO_BLK)
  set(OSFMK_BUILD_${_part} ON CACHE BOOL "")
endforeach()
