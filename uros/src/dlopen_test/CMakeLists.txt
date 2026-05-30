cmake_minimum_required(VERSION 3.16)
project(dlopen_test C)

# dlopen_test — end-to-end validation of musl dlfcn on Uros
# (#234 Phase 7 increment 4).
#
# Same dynamic-link recipe as hello_dyn, plus libfoo.so as a runtime
# dependency loaded via dlopen() — so the build only needs the
# libfoo TARGET (the .so is opened by name on the target FS, not
# linked at build time).

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX  ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR      ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin)
set(DLOPEN_TEST  ${OUT_DIR}/dlopen_test)

execute_process(
  COMMAND cc -m32 -print-libgcc-file-name
  OUTPUT_VARIABLE DLOPEN_TEST_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(DLOPEN_TEST_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/dlopen_test.manifest)
set(DLOPEN_TEST_CMF      ${CMAKE_CURRENT_BINARY_DIR}/dlopen_test.cmf)

add_custom_command(
  OUTPUT ${DLOPEN_TEST}
  COMMAND cc -m32 -nostdinc -fPIE -pie -fno-stack-protector
            -I${MUSL_PREFIX}/include
            -nostdlib
            ${MUSL_PREFIX}/lib/Scrt1.o
            ${MUSL_PREFIX}/lib/crti.o
            ${CMAKE_CURRENT_SOURCE_DIR}/dlopen_test.c
            -L${MUSL_PREFIX}/lib -lc
            ${DLOPEN_TEST_LIBGCC}
            ${MUSL_PREFIX}/lib/crtn.o
            -Wl,-dynamic-linker,/lib/ld-musl-i386.so.1
            -Wl,--build-id=none
            -o ${DLOPEN_TEST}
  COMMAND $<TARGET_FILE:mkmanifest> -o ${DLOPEN_TEST_CMF} ${DLOPEN_TEST_MANIFEST}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${DLOPEN_TEST_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            ${DLOPEN_TEST}
  DEPENDS dlopen_test.c ${DLOPEN_TEST_MANIFEST}
          mkmanifest musl_umbrella libfoo
  COMMENT "Linking dlopen_test (dynamic PIE; dlopens libfoo.so at runtime)"
  VERBATIM
)

add_custom_target(dlopen_test ALL DEPENDS ${DLOPEN_TEST})
