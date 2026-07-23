cmake_minimum_required(VERSION 3.16)
project(hello_dyn C)

# hello_dyn — first dynamically-linked Uros binary (#234 Phase 7).
#
# ET_DYN/PIE with PT_INTERP=/lib/ld-musl-i386.so.1 and DT_NEEDED libc.so,
# linked against the umbrella libc.so.  Built via a custom command (not
# add_executable) because it needs musl's PIE crt + the dynamic link recipe.
# Requires UROS_BUILD_MUSL (the umbrella is the interpreter).

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR     ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin)
set(HELLO_DYN   ${OUT_DIR}/hello_dyn)

# Matching multilib libgcc for compiler builtins musl leaves to the runtime.
execute_process(
  COMMAND cc -m32 -print-libgcc-file-name
  OUTPUT_VARIABLE HELLO_DYN_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(HELLO_DYN_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/hello_dyn.manifest)
set(HELLO_DYN_CMF      ${CMAKE_CURRENT_BINARY_DIR}/hello_dyn.cmf)

add_custom_command(
  OUTPUT ${HELLO_DYN}
  # Dynamic PIE link against the umbrella libc.so.  -dynamic-linker stamps
  # PT_INTERP; -lc resolves the standard libc symbols + adds DT_NEEDED libc.so.
  COMMAND cc -m32 -nostdinc -fPIE -pie -fno-stack-protector
            -I${MUSL_PREFIX}/include
            -nostdlib
            ${MUSL_PREFIX}/lib/Scrt1.o
            ${MUSL_PREFIX}/lib/crti.o
            ${CMAKE_CURRENT_SOURCE_DIR}/hello_dyn.c
            -L${MUSL_PREFIX}/lib -lc
            ${HELLO_DYN_LIBGCC}
            ${MUSL_PREFIX}/lib/crtn.o
            -Wl,-dynamic-linker,/lib/ld-musl-i386.so.1
            -Wl,--build-id=none
            -o ${HELLO_DYN}
  # Embed the .uros_manifest section (same convention as hello_exec).
  COMMAND $<TARGET_FILE:mkmanifest> -o ${HELLO_DYN_CMF} ${HELLO_DYN_MANIFEST}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${HELLO_DYN_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            ${HELLO_DYN}
  DEPENDS hello_dyn.c ${HELLO_DYN_MANIFEST}
          mkmanifest musl_umbrella
  COMMENT "Linking hello_dyn (dynamic PIE against umbrella libc.so)"
  VERBATIM
)

add_custom_target(hello_dyn ALL DEPENDS ${HELLO_DYN})
