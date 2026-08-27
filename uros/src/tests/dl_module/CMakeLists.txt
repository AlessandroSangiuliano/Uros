cmake_minimum_required(VERSION 3.16)
project(dl_module C)

# The shared object dl_test loads (#423).
#
# ⚠️ The link flags are the driver plug-ins' flags, not a simplified set: it is
# `-shared -nostdlib --unresolved-symbols=ignore-all --hash-style=sysv' that
# decides which relocations the linker emits, and a module built differently
# from the ones the servers actually load would test a different loader path.
# --unresolved-symbols is what turns the call to dl_host_seed() into a PLT
# entry instead of a link error.
add_library(dl_module SHARED dl_module.c)

target_compile_options(dl_module PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -fno-builtin -include stdint.h -fPIC
  -fno-stack-protector
)

set_target_properties(dl_module PROPERTIES
  OUTPUT_NAME "dl_module"
  PREFIX ""
  SUFFIX ".so"
  LINK_FLAGS "-shared ${UROS_ARCH_FLAGS_STR} -nostdlib -Wl,--build-id=none -Wl,--unresolved-symbols=ignore-all -Wl,--hash-style=sysv"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/test"
)
