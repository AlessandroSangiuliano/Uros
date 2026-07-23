cmake_minimum_required(VERSION 3.16)
project(hello_dyn_world C)

# hello_dyn_world (#289) — dynamic twin of hello_world: printf + return 0,
# linked as an ET_DYN/PIE against the umbrella libc.so with PT_INTERP set
# to /lib/ld-musl-i386.so.1.  Verifies the DYNAMIC clean-exit path
# (ld-musl -> __libc_start_main -> main -> exit -> task_terminate ->
# ush waitpid).  Same dynamic link recipe as hello_dyn; requires
# UROS_BUILD_MUSL (the umbrella is the interpreter).

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR     ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin)
set(HELLO_DYN_WORLD ${OUT_DIR}/hello_dyn_world)

# Matching multilib libgcc for compiler builtins musl leaves to the runtime.
execute_process(
  COMMAND cc -m32 -print-libgcc-file-name
  OUTPUT_VARIABLE HELLO_DYN_WORLD_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(HELLO_DYN_WORLD_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/hello_dyn_world.manifest)
set(HELLO_DYN_WORLD_CMF      ${CMAKE_CURRENT_BINARY_DIR}/hello_dyn_world.cmf)

add_custom_command(
  OUTPUT ${HELLO_DYN_WORLD}
  # Dynamic PIE link against the umbrella libc.so.  -dynamic-linker stamps
  # PT_INTERP; -lc resolves the standard libc symbols + adds DT_NEEDED libc.so.
  COMMAND cc -m32 -nostdinc -fPIE -pie -fno-stack-protector
            -I${MUSL_PREFIX}/include
            -nostdlib
            ${MUSL_PREFIX}/lib/Scrt1.o
            ${MUSL_PREFIX}/lib/crti.o
            ${CMAKE_CURRENT_SOURCE_DIR}/hello_dyn_world.c
            -L${MUSL_PREFIX}/lib -lc
            ${HELLO_DYN_WORLD_LIBGCC}
            ${MUSL_PREFIX}/lib/crtn.o
            -Wl,-dynamic-linker,/lib/ld-musl-i386.so.1
            -Wl,--build-id=none
            -o ${HELLO_DYN_WORLD}
  # Embed the .uros_manifest section (same convention as hello_dyn).
  COMMAND $<TARGET_FILE:mkmanifest> -o ${HELLO_DYN_WORLD_CMF} ${HELLO_DYN_WORLD_MANIFEST}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${HELLO_DYN_WORLD_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            ${HELLO_DYN_WORLD}
  DEPENDS hello_dyn_world.c ${HELLO_DYN_WORLD_MANIFEST}
          mkmanifest musl_umbrella
  COMMENT "Linking hello_dyn_world (dynamic PIE against umbrella libc.so)"
  VERBATIM
)

add_custom_target(hello_dyn_world ALL DEPENDS ${HELLO_DYN_WORLD})
