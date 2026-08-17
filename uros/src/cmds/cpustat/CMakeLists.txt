cmake_minimum_required(VERSION 3.16)
project(cpustat C)

# cpustat (#375) — per-CPU load monitor.  The first REAL Uros tool linked
# dynamically (ET_DYN/PIE, PT_INTERP=/lib/ld-musl-i386.so.1, DT_NEEDED
# libc.so) rather than statically like ush / ipc_bench / gpustat.  It
# dogfoods the umbrella libc.so as the single Uros runtime: it links -lc
# only and imports the Mach API (mach_host_self / host_processors /
# processor_info) that #375 exports from the umbrella (see the export objects
# in src/contrib/CMakeLists.txt).  No static libmach copy — the RPCs run on
# the umbrella's already-initialised Mach runtime.  Same PIE link recipe as
# hello_dyn.  Requires UROS_BUILD_MUSL (the umbrella is the interpreter).

if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)
set(OUT_DIR     ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin)
set(CPUSTAT     ${OUT_DIR}/cpustat)

# Matching multilib libgcc + its header dir (compiler builtins/headers musl
# leaves to the toolchain; -nostdinc drops the default search path).
execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-libgcc-file-name
  OUTPUT_VARIABLE CPUSTAT_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
  OUTPUT_VARIABLE CPUSTAT_GCC_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CPUSTAT_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/cpustat.manifest)
set(CPUSTAT_CMF      ${CMAKE_CURRENT_BINARY_DIR}/cpustat.cmf)

add_custom_command(
  OUTPUT ${CPUSTAT}
  # Dynamic PIE link against the umbrella libc.so.  -dynamic-linker stamps
  # PT_INTERP; -lc resolves musl + the Uros backend and adds DT_NEEDED
  # libc.so; libmach_core-pic (before -lc) supplies the host_processors /
  # processor_info MIG stubs the umbrella does not carry.
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -nostdinc -fPIE -pie -fno-stack-protector
            -std=gnu11 -fno-builtin -w -include stdint.h
            -I${MUSL_PREFIX}/include
            -I${CPUSTAT_GCC_INCLUDE}
            -I${CMAKE_SOURCE_DIR}/uapi
            -I${CMAKE_BINARY_DIR}/generated/include
            -I${CMAKE_SOURCE_DIR}/src/mach_services/include
            -I${UROS_ARCH_INCLUDE}
            -I${CMAKE_SOURCE_DIR}/src/mach_kernel
            -nostdlib
            ${MUSL_PREFIX}/lib/Scrt1.o
            ${MUSL_PREFIX}/lib/crti.o
            ${CMAKE_CURRENT_SOURCE_DIR}/cpustat.c
            -L${MUSL_PREFIX}/lib -lc
            ${CPUSTAT_LIBGCC}
            ${MUSL_PREFIX}/lib/crtn.o
            -Wl,-dynamic-linker,/lib/ld-musl-i386.so.1
            -Wl,--build-id=none
            -o ${CPUSTAT}
  # Embed the .uros_manifest section (same convention as hello_dyn).
  COMMAND $<TARGET_FILE:mkmanifest> -o ${CPUSTAT_CMF} ${CPUSTAT_MANIFEST}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${CPUSTAT_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            ${CPUSTAT}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cpustat.c ${CPUSTAT_MANIFEST}
          mkmanifest musl_umbrella
          mig_mach_public_headers
  COMMENT "Linking cpustat (dynamic PIE against umbrella libc.so + libmach_core-pic)"
  VERBATIM
)

add_custom_target(cpustat ALL DEPENDS ${CPUSTAT})
