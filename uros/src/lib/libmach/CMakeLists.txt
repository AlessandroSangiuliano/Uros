cmake_minimum_required(VERSION 3.16)
project(libmach C)

# ===========================================================================
# The Mach trap stubs, alone, on a target whose userland is not up yet (#426)
# ===========================================================================
#
# The rest of this file builds a C library: MIG stubs generated from eighteen
# .defs, a startup whose entry trampoline is written in 32-bit assembly, a
# printf, a malloc.  None of that is ready on a new architecture, and none of
# it is what blocks the port -- what blocks the port is that nothing can call
# the kernel at all.
#
# So this half builds the ONE file that is the trap surface.  mach_traps.s is
# `#include <mach/syscall_sw.h>' and nothing else: the list of every trap in
# the system, expanded by the macros in <mach/machine/syscall_sw.h>, which is
# where the whole of #426 lives.  A page of generated stubs, no C, no libc, no
# startup -- linkable by a freestanding image today.
#
# ⚠️ It is a STAGE, and it is named so it cannot be mistaken for the library.
# The day libmach itself builds here (#429, #431) this branch goes and the
# consumers move to `libmach'.  Until then a target that links libmach_traps
# is saying "I want to reach the kernel and I am bringing my own everything
# else", which is exactly what boot_probe is.
if(NOT UROS_TARGET_ARCH STREQUAL "i386")
  set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)
  set(_E ${CMAKE_SOURCE_DIR}/export/include)
  set(_U ${UROS_UAPI_DIR})

  set(_traps_src ${CMAKE_CURRENT_SOURCE_DIR}/mach_traps.s)
  set(_traps_pp  ${CMAKE_CURRENT_BINARY_DIR}/generated/asm/mach_traps.s.S)

  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/asm)

  # Same include path the i386 half uses below, minus the parts that only
  # exist once the MIG stubs are generated -- these stubs need none of them.
  # ⚠️ ${UROS_ARCH_INCLUDE} is what makes <mach/machine/syscall_sw.h> and
  # <machine/syscall/syscall.h> resolve to THIS architecture: it holds the
  # two symlinks the top-level CMakeLists creates per build directory.
  add_custom_command(
    OUTPUT ${_traps_pp}
    COMMAND ${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__
            -I${UROS_ARCH_INCLUDE}
            -I${CMAKE_SOURCE_DIR}/src/mach_kernel
            -I${CMAKE_SOURCE_DIR}/export/include
            ${_traps_src} -o ${_traps_pp}
    DEPENDS ${_traps_src}
            ${CMAKE_SOURCE_DIR}/export/include/mach/syscall_sw.h
            ${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/${UROS_TARGET_ARCH}/syscall_sw.h
    COMMENT "Preprocessing the Mach trap stubs for ${UROS_TARGET_ARCH}"
    VERBATIM
  )
  set_source_files_properties(${_traps_pp} PROPERTIES GENERATED TRUE)

  # ── The MIG stub surface (#426) ──────────────────────────────────────
  #
  # Everything in libmach above the traps reaches <mach/mach_interface.h>,
  # which is generated from mach.defs and does not exist until mig has run.
  # That one missing header is what stops mach_init.c, mig_support.c and the
  # mach_msg_* wrappers -- and it reports itself as a missing FILE, which
  # names the symptom rather than the generator nobody invoked.
  set(UROS_MIG_USERLAND_INCLUDES
      -I${_E}
      -I${UROS_ARCH_INCLUDE}
      -I${_K}
      -I${_E}/mach_debug)

  set(_MIG_OUT ${CMAKE_BINARY_DIR}/generated)
  set(_DEFS
    ${_U}/mach/mach.defs
    ${_U}/mach/mach_port.defs
    ${_U}/mach/mach_host.defs
    ${UROS_UAPI_DIR}/mach/clock.defs
    ${_U}/mach/bootstrap.defs
    ${_U}/mach/exc.defs
    ${UROS_UAPI_DIR}/mach/notify.defs
    # device.defs used to be held out of this list: its stubs failed their own
    # _Static_asserts at -m64, because the two .defs trees described
    # security_token_t differently and were four bytes apart here.  #470 fixed
    # that in mach_types.defs -- `struct[2] of', which says what the C type is
    # -- and this is the line that makes the repair reachable from a clean
    # tree.  ⚠️ It was missing while the commit that closed #470 said device
    # generated: what generated was a HAND RUN of migcom whose output stayed in
    # the build directory, so printf.c compiled against a header no rule
    # produced and the stubs it calls were in no archive.
    ${UROS_UAPI_DIR}/device/device.defs
    # mach_klog.defs: bootstrap's log_forwarder includes <mach/mach_klog.h>.
    ${_U}/mach/mach_klog.defs
    # mach_debug.defs: bootstrap's elf.c calls host_load_symbol_table.
    #
    # 🔥 This line was held out with a comment saying migcom had a defect on
    # this target, and that was WRONG -- migcom was right and the .defs lied.
    # Its stubs failed two of their own assertions:
    #
    #   "MIG measures the out-of-line info in the wrong unit"
    #   "MIG and the compiler disagree on the size of region"
    #
    # Both name a type whose size follows the pointer width, declared in
    # mach_debug_types.defs as a fixed count of 32-bit words: vm_info_region_t
    # is 40 bytes on i386 and 56 here, vm_info_object_t 84 and 120.  Four more
    # beside them were equally wrong.  Same class as #470, in another file, and
    # the assertion machinery of #416 doing exactly the job it was added for --
    # naming the wrong number rather than sending it.
    ${_U}/mach_debug/mach_debug.defs
  )
  set(LIBMACH_MIG_GENERATED)
  foreach(d IN LISTS _DEFS)
    add_mig_userland(${d} ${_MIG_OUT} LIBMACH_MIG)
  endforeach()

  # #169: published under the names consumers actually include.
  set(_PUB "${CMAKE_BINARY_DIR}/generated/include/mach")
  file(MAKE_DIRECTORY "${_PUB}")
  # ⚠️ i386 publishes eighteen of these and this published nine, which is not a
  # judgement about what the target needs -- it is the list that happened to be
  # written when only libmach itself consumed them.  bootstrap wants two more:
  # the SERVER half of bootstrap.defs, which it demultiplexes, and mach_klog.
  set(_PUB_MAP  mach_interface.h mach_interface.h  mach_port.h mach_port.h
                mach_host.h mach_host.h  clock.h clock.h
                bootstrap.h bootstrap.h  exc.h exc_user.h
                exc_server.h exc_server.h  notify_server.h notify_server.h
                bootstrap_server.h bootstrap_server.h
                mach_klog.h mach_klog.h)
  # device/device.h goes under its own directory, which is how it is spelled.
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/device")
  set(_PUB_HDRS)
  list(LENGTH _PUB_MAP _n_map)
  math(EXPR _last "${_n_map} - 1")
  foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _PUB_MAP ${_i} _from)
    list(GET _PUB_MAP ${_j} _to)
    add_custom_command(OUTPUT ${_PUB}/${_to}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${_MIG_OUT}/${_from} ${_PUB}/${_to}
      DEPENDS ${_MIG_OUT}/${_from}
      COMMENT "Publish mach/${_to} (from ${_from})" VERBATIM)
    list(APPEND _PUB_HDRS ${_PUB}/${_to})
  endforeach()
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/include/device/device.h
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_MIG_OUT}/device.h ${CMAKE_BINARY_DIR}/generated/include/device/device.h
    DEPENDS ${_MIG_OUT}/device.h
    COMMENT "Publish device/device.h" VERBATIM)
  list(APPEND _PUB_HDRS ${CMAKE_BINARY_DIR}/generated/include/device/device.h)

  # ⚠️ The SAME NAME the i386 half uses, and that is the whole point.  It was
  # mig_mach_headers_x86_64, so every consumer had to know which target it was
  # built for -- and 41 CMakeLists in this tree name mig_mach_public_headers.
  # Making each of them choose would have been forty-one copies of a
  # conditional; one name for one role is correct by construction, and only one
  # of the two halves is ever configured.
  add_custom_target(mig_mach_public_headers DEPENDS ${_PUB_HDRS})

  # And the device header under the name its consumers already use.  ⚠️ Same
  # argument: libblk and libsa_fs depend on `mig_device_header', which the i386
  # tree defines beside its own device.defs rule.  Publishing the identical
  # header under a different target name would make every consumer choose.
  add_custom_target(mig_device_header
    DEPENDS ${CMAKE_BINARY_DIR}/generated/include/device/device.h)

  # ── The library itself ───────────────────────────────────────────────
  #
  # Every one of libmach's C sources compiles for this target except strtod.c,
  # which i386 does not build either.
  #
  # crt0.c and stack_protector.c came in with #426: both carried a machine's
  # assembly inside C, and both now carry two.  They are written twice rather
  # than parameterised because the sequences differ in kind and not in width
  # -- the entry argument travels on the stack on one and in a register on the
  # other -- and because a `%esp' that means half a register is the sort of
  # thing an assembler should refuse rather than a macro should paper over.
  #
  # printf.c is in, and with it the reason it was ever out: it reaches the
  # console through <device/device.h>, and crt0 pulls exit, which pulls panic,
  # which calls printf -- so until device.defs generated here, no program on
  # this target could be entered through __start_mach at all.
  #
  # ⚠️ Compiling printf.c is only half of that.  It CALLS device_open and
  # device_write_inband, which live in the stubs device.defs generates, and an
  # archive that holds the caller and not the callee links for exactly as long
  # as nobody calls printf.  The two arrive together above or neither is worth
  # anything.
  file(GLOB _MACH_C CONFIGURE_DEPENDS "*.c")
  file(GLOB _MS_WRAPPERS CONFIGURE_DEPENDS "ms_*.c")
  list(REMOVE_ITEM _MACH_C ${_MS_WRAPPERS})
  list(REMOVE_ITEM _MACH_C
    ${CMAKE_CURRENT_SOURCE_DIR}/strtod.c)           # needs float.h macros

  add_library(libmach STATIC ${_traps_pp} ${_MACH_C} ${LIBMACH_MIG_GENERATED})
  add_dependencies(libmach mig_mach_public_headers)
  set_target_properties(libmach PROPERTIES
    OUTPUT_NAME "mach"
    ARCHIVE_OUTPUT_DIRECTORY
      "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/lib")

  target_compile_options(libmach PRIVATE ${UROS_ARCH_FLAGS})
  # ⚠️ -mstack-protector-guard=global, and the i386 half below has always had
  # it.  Without it gcc reads the canary from %fs:0x28 -- the TLS slot, which
  # is the sane default on a system whose libc installs a TCB before any
  # protected function runs.  Here nothing does: this library IS the startup,
  # FS base is zero when mach_init is called, and the first canary prologue
  # reads address 0x28 and takes a page fault at the second instruction of the
  # first function crt0 calls.
  #
  # 🔥 stack_protector.c defines __stack_chk_guard and crt0 calls
  # init_stack_guard() to fill it -- a global canary, written on every startup
  # and read by nothing, because the compiler was emitting the other kind.
  # The two halves were consistent with each other and with nothing in
  # between; on i386 the flag is what joined them, and it was never copied.
  target_compile_options(libmach PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -D_BSD -fno-builtin -w
                            -mstack-protector-guard=global -include stdint.h>)


  # ⚠️ The MIG OUTPUT directory is deliberately NOT here.  It holds a file
  # called mach.h -- the header for mach.defs -- and libmach has its own
  # mach.h, the umbrella that pulls in mach_init.h and with it the
  # `#define mach_task_self() mach_task_self_' every one of these files
  # needs.  Put the raw directory on the path and the generated one wins,
  # the macro disappears, and seven sources fail with an implicit
  # declaration that names a function nobody ever calls.  The generated
  # stubs still find their own headers: they include them with quotes, and
  # a quoted include looks in the including file's own directory first.
  target_include_directories(libmach PUBLIC
    ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/uapi
    ${CMAKE_BINARY_DIR}/generated/include
    ${CMAKE_SOURCE_DIR}/src/mach_services/include
    ${CMAKE_SOURCE_DIR}/export/include
    ${UROS_ARCH_INCLUDE}
    ${CMAKE_SOURCE_DIR}/src/mach_kernel
    ${CMAKE_SOURCE_DIR}/include)

  return()
endif()

# Raccolgo le sorgenti locali (only .c and .S; lowercase .s needs preprocessing)
file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S")
file(GLOB TOP_S_LOWER_S CONFIGURE_DEPENDS "*.s")

# Exclude ms_*.c wrappers: they call mig_<name>() variants that require a sed-based
# post-processing rename of MIG output (the original common.mk did this).  Without
# the rename, the MIG stubs provide the public names (vm_allocate, etc.) directly,
# which is sufficient for the bootstrap server and standalone Mach programs.
file(GLOB MS_WRAPPERS CONFIGURE_DEPENDS "ms_*.c")
list(REMOVE_ITEM LIB_SOURCES ${MS_WRAPPERS})

# Exclude C implementations replaced by SSE2 assembly in i386/ (#106 import
# from libsa_mach).
list(FILTER LIB_SOURCES EXCLUDE REGEX "(bcopy|memcpy|memmove|memset)\\.c$")

# sprintf.c (imported from libsa_mach) provides sprintf/vsprintf/snprintf/
# vsnprintf.  printf.c (also from libsa_mach) used to also define sprintf/
# vsprintf — now stripped from printf.c after #106 since the duplicates
# collided once --allow-multiple-definition was removed.

# strtod.c needs float.h macros (DBL_DIG, HUGE_VAL, ...) that our local
# sa_mach/i386/float.h does not provide.  No userspace target calls strtod
# today, so excluding it costs nothing.  Re-enable when the float.h gap is
# closed (or when musl lands and replaces this file wholesale).
list(REMOVE_ITEM LIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/strtod.c)

# Per-file: strtod.c needs math.h (frexp, ldexp, modf, ...) — imported from
# libsa_mach in #106.  Other files only need stdint.h.

# __NO_UNDERSCORES__: ELF does not prefix symbols with '_' (unlike Mach-O/a.out)
set(ASM_PREPROCESS_INCLUDES "-D__NO_UNDERSCORES__ -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_BINARY_DIR}/generated/include -I${CMAKE_SOURCE_DIR}/src/mach_services/include -I${CMAKE_SOURCE_DIR}/export/include -I${UROS_ARCH_INCLUDE} -I${CMAKE_SOURCE_DIR}/src/mach_kernel -I${CMAKE_SOURCE_DIR}/include")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/asm")

# Preprocess ALL lowercase .s files (they use #include and need CPP)
foreach(sfile IN LISTS TOP_S_LOWER_S)
  get_filename_component(sname ${sfile} NAME)
  set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
  add_custom_command(
    OUTPUT ${pp_output}
    COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} | sed -e 's#/ # #g' -e 's/\\.align ALIGN/.align 2/g' -e 's/^ \\+zero /# zero /g' > ${pp_output}"
    DEPENDS ${sfile}
            ${CMAKE_SOURCE_DIR}/export/include/mach/syscall_sw.h
    COMMENT "Preprocessing asm ${sfile}"
    VERBATIM
  )
  set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
  list(APPEND LIB_SOURCES ${pp_output})
endforeach()

# Aggiungo sorgenti specifiche per architettura i386 se presenti
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/i386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "i386/*.c" "i386/*.S")
  file(GLOB ARCH_S_LOWER_S CONFIGURE_DEPENDS "i386/*.s")
  # bzero.s is the legacy non-SSE bzero — memset.s (imported from libsa_mach
  # in #106) provides both memset AND bzero with SSE2.  Drop the duplicate.
  list(FILTER ARCH_S_LOWER_S EXCLUDE REGEX "/bzero\\.s$")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
  foreach(sfile IN LISTS ARCH_S_LOWER_S)
    get_filename_component(sname ${sfile} NAME)
    set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
    add_custom_command(
      OUTPUT ${pp_output}
      COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} | sed -e 's#/ # #g' -e 's/\\.align ALIGN/.align 2/g' -e 's/^ \\+zero /# zero /g' > ${pp_output}"
      DEPENDS ${sfile}
      COMMENT "Preprocessing asm ${sfile}"
      VERBATIM
    )
    set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
    list(APPEND LIB_SOURCES ${pp_output})
  endforeach()
endif()

# Generate MIG user stubs from kernel .defs BEFORE creating the library target
# (add_library snapshots the source list; appending afterwards has no effect)
#
# Explicit list matching the original common.mk MIG_DEFS (17 .defs files).
# Do NOT glob — the kernel tree contains extra .defs (prof.defs, etc.) that
# either fail MIG parsing or are not used by userspace.

set(_KERNEL ${CMAKE_SOURCE_DIR}/src/mach_kernel)
set(_EXPORT ${CMAKE_SOURCE_DIR}/export/include)
set(_UAPI   ${UROS_UAPI_DIR})
# MIG include path: export tree first (userspace .defs), then kernel for extras
set(_MIG_INC ${_UAPI} ${_EXPORT} ${UROS_ARCH_INCLUDE} ${_KERNEL} ${_UAPI}/mach_debug ${_EXPORT}/mach_debug)

# --- All .defs userland needs (matching original common.mk MIG_DEFS) ---
# ⚠️ This list used to say it took the export tree on purpose, because "the
# kernel tree's .defs have kernel-internal definitions (alarm_port_t, SEQNOS
# conditionals, etc.) that produce incompatible MIG output for userspace".
# The two are one file now (#471), and the claim did not survive being
# tested: alarm_port_t carries an `#else ctype: mach_port_t' that hands
# userland exactly what the export copy handed it, and the SEQNOS difference
# was `#endif SEQNOS' against `#endif' -- trailing tokens on a directive, not
# a branch.  What settles it is that the generated stubs did not move.
set(EXPORT_MIG_DEFS
  ${_UAPI}/mach/bootstrap.defs
  ${UROS_UAPI_DIR}/mach/clock.defs
  ${_UAPI}/mach/clock_reply.defs
  ${UROS_UAPI_DIR}/mach/default_pager_object.defs
  ${_UAPI}/mach/exc.defs
  ${_UAPI}/mach/ledger.defs
  ${_UAPI}/mach/mach.defs
  ${_UAPI}/mach/mach_host.defs
  ${_UAPI}/mach/mach_klog.defs
  ${UROS_UAPI_DIR}/mach/mach_norma.defs
  ${_UAPI}/mach/mach_port.defs
  ${_UAPI}/mach/memory_object.defs
  ${_UAPI}/mach/memory_object_default.defs
  ${UROS_UAPI_DIR}/mach/notify.defs
  ${_UAPI}/mach/sync.defs
  ${_UAPI}/mach_debug/mach_debug.defs
  ${_UAPI}/device/device_request.defs
  ${_UAPI}/device/device_reply.defs
)
foreach(DEFS IN LISTS EXPORT_MIG_DEFS)
  add_mig_defs(${DEFS} ${_MIG_INC})
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()

# --- #169: publish MIG-generated user/server headers as <mach/NAME.h> --------
# Single source of truth: .defs files generate headers into build/generated/
# (done above by add_mig_defs).  Here we copy them into the public include
# path build/generated/include/mach/ so every userspace target that has
# generated/include in its include search path can resolve <mach/bootstrap.h>
# etc. from the regenerated copy instead of a checked-in stale one.
#
# Rows: <build-generated filename>  <public filename under mach/>
# One rename preserves a legacy public name to avoid a source-wide rename:
#   memory_object_default_user.h -> memory_object_default.h
set(_PUB_MACH_DIR "${CMAKE_BINARY_DIR}/generated/include/mach")
file(MAKE_DIRECTORY "${_PUB_MACH_DIR}")

set(_MIG_PUBLIC_MAP
  # Build filename from add_mig_defs is <defs>.h / <defs>_server.h.
  # Public filename is the legacy name consumers actually #include.
  # user-mode
  bootstrap.h                    bootstrap.h
  clock.h                        clock.h
  clock_reply.h                  clock_reply.h
  default_pager_object.h         default_pager_object.h
  exc.h                          exc_user.h
  ledger.h                       ledger.h
  mach_host.h                    mach_host.h
  mach_klog.h                    mach_klog.h
  mach_interface.h               mach_interface.h
  mach_norma.h                   mach_norma.h
  mach_port.h                    mach_port.h
  memory_object_default.h        memory_object_default.h
  sync.h                         sync.h
  # server-mode
  bootstrap_server.h             bootstrap_server.h
  exc_server.h                   exc_server.h
  memory_object_default_server.h memory_object_default_server.h
  memory_object_server.h         memory_object_server.h
  notify_server.h                notify_server.h
)

set(_MIG_PUBLIC_HEADERS)
list(LENGTH _MIG_PUBLIC_MAP _map_len)
math(EXPR _map_last "${_map_len} - 1")
foreach(_i RANGE 0 ${_map_last} 2)
  math(EXPR _j "${_i} + 1")
  list(GET _MIG_PUBLIC_MAP ${_i} _src_name)
  list(GET _MIG_PUBLIC_MAP ${_j} _dst_name)
  set(_src "${CMAKE_BINARY_DIR}/generated/${_src_name}")
  set(_dst "${_PUB_MACH_DIR}/${_dst_name}")
  add_custom_command(
    OUTPUT ${_dst}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_src} ${_dst}
    DEPENDS ${_src}
    COMMENT "Publish mach/${_dst_name} (from ${_src_name})"
    VERBATIM
  )
  list(APPEND _MIG_PUBLIC_HEADERS ${_dst})
endforeach()

add_custom_target(mig_mach_public_headers DEPENDS ${_MIG_PUBLIC_HEADERS})

# device.defs: the parent CMakeLists already generates device_user.c / device_server.c
# (and the public device/device.h header) via a standalone custom command, so we
# just reference those outputs here to avoid duplicate-rule errors.
list(APPEND LIB_SOURCES
  ${CMAKE_BINARY_DIR}/generated/device_user.c
  ${CMAKE_BINARY_DIR}/generated/device_server.c
)
set_source_files_properties(
  ${CMAKE_BINARY_DIR}/generated/device_user.c
  ${CMAKE_BINARY_DIR}/generated/device_server.c
  PROPERTIES GENERATED TRUE
)

add_library(libmach STATIC ${LIB_SOURCES})

# #469: which trap mach_msg() reaches the kernel through.  OFF is the narrow
# six-argument urmach_msg; ON restores the nine-argument overwrite form
# verbatim, so that the two can be built and measured against each other.
#
# ⚠️ On mach_msg.c ONLY.  A definition applied to the whole library would
# rebuild eighty objects between the A and the B of a measurement, and the
# difference would no longer be one function.
option(UROS_MACH_MSG_WIDE "mach_msg() uses the nine-argument trap (#469)" OFF)
set_source_files_properties(mach_msg.c PROPERTIES COMPILE_DEFINITIONS
  "$<$<BOOL:${UROS_MACH_MSG_WIDE}>:UROS_MACH_MSG_WIDE>")

# Depend on MIG-generated device/device.h (defined in parent CMakeLists)
add_dependencies(libmach mig_device_header mig_mach_public_headers)

# Compat: il codice è storico (K&R/gnu89); impostiamo opzioni less strict per compilare
# Apply C-only options (including -include stdint.h) only when compiling C sources
target_compile_options(libmach PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -D_BSD -fno-builtin -w -mstack-protector-guard=global -include stdint.h>)

# Build assembly files and C as 32-bit where needed
target_compile_options(libmach PRIVATE ${UROS_ARCH_FLAGS} -msse -msse2)
target_compile_options(libmach PRIVATE ${UROS_ARCH_FLAGS})

# Work around GCC constprop + SSP + va_list code generation bug on i386:
# GCC creates a constprop clone of _doprnt where the va_list calling
# convention is inconsistent between the inlined printf and standalone
# vprintf callers.  Disable SSP for printf.c until GCC is fixed.
# (Imported from libsa_mach via #106.)
set_source_files_properties(printf.c PROPERTIES COMPILE_FLAGS -fno-stack-protector)

# Mach userspace headers (sa_mach) solo per questa libreria
target_include_directories(libmach BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
)
target_include_directories(libmach PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/uapi			# #179 — UAPI split
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Metto gli archivi in export/<system>/lib per compatibilità con l'albero esistente
set_target_properties(libmach PROPERTIES
  OUTPUT_NAME "mach"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)

# ===========================================================================
# libmach_core (#251) — slim libmach for servers that link musl libc.
#
# Same source set as libmach minus every file that defines a symbol musl
# already provides.  Used by servers migrated to the musl libc path so
# they can link without -Wl,--allow-multiple-definition.  When the last
# server is migrated (Phase 8 of #219), the excluded files are deleted
# from libmach itself and libmach_core gets renamed back to libmach.
#
# Gated by UROS_BUILD_MUSL: no point building a duplicate archive for
# users not on the musl path.
# ===========================================================================
if(UROS_BUILD_MUSL)
  set(LIBMACH_MUSL_CONFLICTS
    # stdio (musl src/stdio/, src/string/__doprintf)
    "/printf\\.c$"
    "/sprintf\\.c$"
    "/doprnt\\.c$"
    "/fprintf_stderr\\.c$"
    # malloc family (musl src/malloc/mallocng)
    "/malloc\\.c$"
    "/sbrk\\.c$"
    # process termination (musl src/exit/)
    "/exit\\.c$"
    "/panic\\.c$"          # abort wrapper, calls printf+exit
    "/Ovfork\\.c$"         # defines vfork(); musl has its own
    # SSP (musl src/env/__stack_chk_fail.c); init_stack_guard stub
    # is provided by libposix-uros so crt0.c still links cleanly.
    "/stack_protector\\.c$"
    # error reporting (musl src/errno/, src/string/strerror.c)
    "/errno\\.c$"
    "/error_codes\\.c$"
    # POSIX libc bits (musl src/{ctype,env,unistd,stdlib}/)
    "/ctype\\.c$"
    "/getenv\\.c$"
    "/sleep\\.c$"
    # string family (musl src/string/) — both the consolidated
    # sa_string.c and the per-function .c files
    "/sa_string\\.c$"
    "/strcat\\.c$"   "/strcmp\\.c$"   "/strcpy\\.c$"
    "/strlen\\.c$"   "/strspn\\.c$"
    "/strncat\\.c$"  "/strncmp\\.c$"  "/strncpy\\.c$"
    "/strlcat\\.c$"  "/strlcpy\\.c$"
    # numeric conversion (musl src/stdlib/strto*.c)
    "/strtol\\.c$"   "/strtoul\\.c$"
    "/strtoq\\.c$"   "/strtouq\\.c$"
    # i386 assembly conflicts
    "/i386/memset\\.s$"     # memset, bzero  — musl provides
    "/i386/memcpy\\.s$"     # memcpy, memmove, bcopy
    "/i386/ffs\\.s$"        # ffs
    "/i386/gcc\\.s$"        # __divsi3 / __udivsi3 (libgcc provides)
    "/i386/fork\\.s$"       # fork() syscall stub
  )

  set(LIBMACH_CORE_SOURCES ${LIB_SOURCES})
  foreach(_pat IN LISTS LIBMACH_MUSL_CONFLICTS)
    list(FILTER LIBMACH_CORE_SOURCES EXCLUDE REGEX ${_pat})
  endforeach()

  # The preprocessed i386 .s outputs were appended after running through
  # cpp into ${CMAKE_CURRENT_BINARY_DIR}/generated/asm/*.S.  Filter those
  # too so the i386 stubs above don't sneak back in via the preprocessed
  # path.
  list(FILTER LIBMACH_CORE_SOURCES EXCLUDE REGEX "/generated/asm/(memset|memcpy|ffs|gcc|fork|bzero)\\.s\\.S$")

  add_library(libmach_core STATIC ${LIBMACH_CORE_SOURCES})
  add_dependencies(libmach_core mig_device_header mig_mach_public_headers)

  # Mirror libmach's compile + include configuration so the two stay
  # ABI-compatible (same gnu11, same SSE2, same SSP guard mode).
  target_compile_options(libmach_core PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -D_BSD -fno-builtin -w
                            -mstack-protector-guard=global -include stdint.h>
  )
  target_compile_options(libmach_core PRIVATE ${UROS_ARCH_FLAGS} -msse -msse2)
  target_compile_options(libmach_core PRIVATE
    ${UROS_ARCH_FLAGS}
  )

  target_include_directories(libmach_core BEFORE PUBLIC
    ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  )
  target_include_directories(libmach_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated/include
    ${CMAKE_SOURCE_DIR}/uapi
    ${CMAKE_SOURCE_DIR}/src/mach_services/include
    ${CMAKE_SOURCE_DIR}/export/include
    ${UROS_ARCH_INCLUDE}
    ${CMAKE_SOURCE_DIR}/src/mach_kernel
    ${CMAKE_SOURCE_DIR}/include
  )

  set_target_properties(libmach_core PROPERTIES
    OUTPUT_NAME "mach_core"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
  )

  # PIC variant for the umbrella libc.so (#234 incr 2b).  Same slim source
  # set as libmach_core (musl provides the excluded libc symbols), compiled
  # position-independent so the Mach traps + RPC stubs can be folded into
  # libc.so via EXTRA_OBJS.  Flags/includes copied from libmach_core so the
  # two never drift.  The Mach trap is lcall/sysenter (immediate-only) and
  # the SSE2 mem*.s use no absolute relocs → PIC-clean.
  add_library(libmach_core-pic STATIC ${LIBMACH_CORE_SOURCES})
  add_dependencies(libmach_core-pic mig_device_header mig_mach_public_headers)
  set_target_properties(libmach_core-pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(libmach_core-pic PRIVATE
    $<TARGET_PROPERTY:libmach_core,COMPILE_OPTIONS>)
  target_include_directories(libmach_core-pic PRIVATE
    $<TARGET_PROPERTY:libmach_core,INCLUDE_DIRECTORIES>)
  set_target_properties(libmach_core-pic PROPERTIES OUTPUT_NAME "mach_core-pic")

  # #396: the two objects the musl umbrella must receive LOOSE (see
  # src/contrib/CMakeLists.txt — its --exclude-libs=ALL hides every archive
  # member, and only loose objects keep the Mach surface exported for
  # dynamic tools like cpustat, #375).  Compiled here as an OBJECT library
  # so the umbrella names them as $<TARGET_OBJECTS:...> instead of guessing
  # this target's internal CMakeFiles object paths — which encoded the
  # source depth as __/ segments and broke the day libmach moved.
  add_library(libmach_export_objs OBJECT
    ${CMAKE_BINARY_DIR}/generated/mach_host_user.c
    ${CMAKE_CURRENT_BINARY_DIR}/generated/asm/mach_traps.s.S
  )
  add_dependencies(libmach_export_objs mig_device_header mig_mach_public_headers)
  set_target_properties(libmach_export_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(libmach_export_objs PRIVATE
    $<TARGET_PROPERTY:libmach_core,COMPILE_OPTIONS>)
  target_include_directories(libmach_export_objs PRIVATE
    $<TARGET_PROPERTY:libmach_core,INCLUDE_DIRECTORIES>)
endif()
