# The x86-64 build the harness runs, as a CMake initial cache (#653).
#
#   cmake -G Ninja -C uros/cmake/presets/x86_64.cmake -S uros -B uros/build-x86_64
#
# Only what differs from the defaults is here.  The x86-64 arm of CMakeLists.txt
# already sets the rest, so a configure from this file has the same OSFMK_* and
# UROS_* options as the builds that used to be configured by copying another
# build's cache, because nothing in the tree said what those options were.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(UROS_TARGET_ARCH x86_64 CACHE STRING "")

# Release turns the kernel symbol table off by default; every build the harness
# has judged had it on.
set(OSFMK_GEN_KSYMS ON CACHE BOOL "")
