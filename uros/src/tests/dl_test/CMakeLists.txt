cmake_minimum_required(VERSION 3.16)
project(dl_test C)

# The module travels INSIDE the test (#423).
#
# 🔑 Not how the servers get theirs -- they fetch by class from bootstrap's
# module pool, which reads /dev/boot_device/mach_servers/modules/ off the disk.
# There is no disk stage on x86-64, and waiting for one would mean the loader
# could not be tested on the target it was ported for.
#
# ⚠️ And it costs nothing in fidelity: a relocation does not know where the
# bytes came from.  What is under test is dl_map_object and dl_relocate, and
# both take a buffer.  The module is built with the driver plug-ins' own link
# flags, which is the part that would matter if it differed.
set(DL_MODULE_SO "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules/test/dl_module.so")
set(DL_MODULE_C  "${CMAKE_CURRENT_BINARY_DIR}/dl_module_blob.c")

add_custom_command(
  OUTPUT ${DL_MODULE_C}
  COMMAND ${CMAKE_COMMAND}
          -DIN=${DL_MODULE_SO}
          -DOUT=${DL_MODULE_C}
          -DSYM=dl_module_blob
          -P ${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake
  DEPENDS dl_module ${DL_MODULE_SO}
  COMMENT "Embedding dl_module.so into dl_test"
  VERBATIM
)

add_executable(dl_test_bin dl_test.c ${DL_MODULE_C})

add_dependencies(dl_test_bin mig_mach_public_headers dl_module)

target_compile_options(dl_test_bin PRIVATE
  -std=gnu11 ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(dl_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(dl_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libdl
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${UROS_LIBPTHREADS_MD}
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# ⚠️ --export-dynamic is load-bearing here and not boilerplate: the module
# calls dl_host_seed(), which lives in this program, and libdl resolves it
# through this program's own dynamic symbol table (dl_bootstrap_self).  Without
# it the symbol is not there to be found and arm [3] fails for a reason that
# has nothing to do with the relocation it is testing.
target_link_libraries(dl_test_bin PRIVATE
  -Wl,--start-group
  libdl
  libpthreads
  libmach
  -Wl,--end-group
)

target_link_options(dl_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--build-id=none
)

set_target_properties(dl_test_bin PROPERTIES
  OUTPUT_NAME "dl_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
