# contrib/ — third-party sources vendored into Uros.
#
# Phase 1 of the libc.so consolidation roadmap (#219, #249):
# musl 1.2.5 is vendored under contrib/musl/ and built standalone via
# its own ./configure + make.  Output: libc-musl.a in ${MUSL_PREFIX}/lib.
#
# No Uros runtime integration yet — Phase 2 introduces __uros_syscall
# and libposix-uros.

if(UROS_BUILD_MUSL)
  include(ExternalProject)

  set(MUSL_SRC      ${CMAKE_CURRENT_SOURCE_DIR}/musl)
  set(MUSL_BUILD    ${CMAKE_CURRENT_BINARY_DIR}/musl-build)
  set(MUSL_PREFIX   ${CMAKE_CURRENT_BINARY_DIR}/musl-install)
  set(MUSL_LIB      ${MUSL_PREFIX}/lib/libc.a)
  set(MUSL_OUT      ${MUSL_PREFIX}/lib/libc-musl.a)
  # Phase 7 (#234 incr 2a): shared build also emits libc.so (which IS the
  # musl dynamic linker, entry _dlstart) and the ld-musl-i386.so.1 symlink.
  # --syslibdir below keeps the symlink inside our install tree instead of
  # the host's absolute /lib.
  set(MUSL_SO       ${MUSL_PREFIX}/lib/libc.so)
  set(MUSL_LDSO     ${MUSL_PREFIX}/lib/ld-musl-i386.so.1)

  # i386 freestanding-ish build.  We still rely on the host gcc with -m32
  # for now; cross-toolchain comes in a later phase.  Shared is enabled
  # (#234 incr 2a) so the build also emits libc.so / ld-musl-i386.so.1
  # (the dynamic linker); libc.a is still produced for the static path.
  # The patched syscall_arch.h routes __syscallN to __uros_syscallN, which
  # libc.so resolves via the WEAK stubs in src/internal/uros_syscall_stub.c
  # (links standalone now; the real dispatcher gets bundled in 2b).
  ExternalProject_Add(musl_extproj
    SOURCE_DIR        ${MUSL_SRC}
    BINARY_DIR        ${MUSL_BUILD}
    INSTALL_DIR       ${MUSL_PREFIX}
    # NOTE on the target/tool selection:
    # - We pass --target=i386-linux-gnu so musl's configure picks ARCH=i386
    #   (without it, on an x86_64 host with `cc -m32`, __ILP32__ is set
    #   and configure mis-detects ARCH=x32, which then fails because the
    #   x32 .s files use 64-bit registers).
    # - We override AR=ar / RANLIB=ranlib so the build uses the native
    #   binutils instead of i386-linux-gnu-{ar,ranlib} (which don't
    #   exist on this host).  CC=cc -m32 produces i386 ELF objects that
    #   the native ar/ranlib handle just fine.
    CONFIGURE_COMMAND ${MUSL_SRC}/configure
                        --srcdir=${MUSL_SRC}
                        --prefix=${MUSL_PREFIX}
                        --syslibdir=${MUSL_PREFIX}/lib
                        --target=i386-linux-gnu
                        --enable-shared
                        CC=cc
                        "CFLAGS=-m32 -Os -pipe -fno-stack-protector"
                        AR=ar
                        RANLIB=ranlib
    BUILD_COMMAND     ${CMAKE_COMMAND} -E env make -j
    INSTALL_COMMAND   make install
                COMMAND ${CMAKE_COMMAND} -E copy ${MUSL_LIB} ${MUSL_OUT}
                # #291: strip the WEAK -ENOSYS syscall/clone/TLS stubs
                # (uros_syscall_stub.lo) from the STATIC libc-musl.a.  They
                # exist only so the shared libc.so / ld-musl links standalone
                # under --no-undefined; in the static archive they are a trap
                # — a static app that links libposix-uros (every Uros binary)
                # would otherwise pick up the weak -ENOSYS __uros_clone /
                # __uros_syscallN instead of the strong dispatcher, breaking
                # pthread_create (EAGAIN), TLS, and every syscall unless each
                # binary force-pulls them with -Wl,-u (the #286/#288 hack).
                # The shared libc.so keeps the stubs (built from libc.a, not
                # this copy), so the standalone ldso link is unaffected.
                COMMAND ar d ${MUSL_OUT} uros_syscall_stub.lo
    BUILD_BYPRODUCTS  ${MUSL_LIB} ${MUSL_OUT} ${MUSL_SO} ${MUSL_LDSO}
    LOG_CONFIGURE     ON
    LOG_BUILD         ON
    LOG_INSTALL       ON
  )

  # Imported target so consumers can do: target_link_libraries(x musl::c)
  add_library(musl::c STATIC IMPORTED GLOBAL)
  set_target_properties(musl::c PROPERTIES
    IMPORTED_LOCATION             ${MUSL_OUT}
    INTERFACE_INCLUDE_DIRECTORIES ${MUSL_PREFIX}/include
  )
  add_dependencies(musl::c musl_extproj)

  # Shared variant (#234 incr 2a): libc.so == ld-musl-i386.so.1, the musl
  # dynamic linker (entry _dlstart).  Exposed as an imported SHARED target
  # so the exec_server / disk-image plumbing can locate the artifact.
  add_library(musl::ldso SHARED IMPORTED GLOBAL)
  set_target_properties(musl::ldso PROPERTIES
    IMPORTED_LOCATION             ${MUSL_SO}
    INTERFACE_INCLUDE_DIRECTORIES ${MUSL_PREFIX}/include
  )
  add_dependencies(musl::ldso musl_extproj)

  message(STATUS "musl 1.2.5 vendored: build=${MUSL_BUILD} install=${MUSL_PREFIX}")
  message(STATUS "musl dynamic linker: ${MUSL_LDSO}")

  # -------------------------------------------------------------------------
  # Umbrella libc.so (#234 incr 2b).
  #
  # musl_extproj (above) builds a libc.so with EXTRA_OBJS empty — the WEAK
  # __uros_syscall stubs satisfy --no-undefined but it is a hollow no-op.
  # Here we relink libc.so folding in the Uros PIC backend so the dynamic
  # linker (== libc.so) carries the real __uros_syscall dispatcher + the
  # Mach/VFS stack.  src/ is processed before src/contrib, so the *-pic
  # targets already exist.
  #
  #   --whole-archive on libposix-uros-pic  -> its STRONG __uros_syscall*
  #     override the weak stubs already in libc.so (a plain reference would
  #     not pull the archive, since the weak defs already satisfy it);
  #   the remaining archives in --start-group/--end-group are pulled by
  #     reference (they have circular inter-dependencies).
  #
  # libc.so is rm'd first because EXTRA_OBJS is not one of its make
  # prerequisites — without that, make would see it up-to-date and skip the
  # relink.  The result is copied over the installed ${MUSL_SO} that the
  # ld-musl-i386.so.1 symlink points at.
  # -------------------------------------------------------------------------
  # #375: --exclude-libs=ALL (from musl's configure) forces every symbol of
  # the folded-in *-pic ARCHIVES to local, so a dynamic tool cannot import the
  # Mach API from the umbrella.  Only LOOSE objects escape --exclude-libs, so
  # we hand the two objects that carry the Mach surface a tool needs — the MIG
  # host stubs (host_processors/processor_info) and the trap thunks
  # (mach_host_self) — to the link as loose objects.  Their globals stay
  # exported, letting cpustat run the umbrella's single, initialised Mach
  # runtime instead of a static duplicate.  The objects come from the
  # dedicated libmach_export_objs OBJECT library (#396) — named through
  # $<TARGET_OBJECTS>, not guessed from another target's CMakeFiles layout.
  set(MACH_EXPORT_OBJS "$<JOIN:$<TARGET_OBJECTS:libmach_export_objs>, >")

  set(MUSL_UMBRELLA_STAMP ${CMAKE_CURRENT_BINARY_DIR}/musl-umbrella.stamp)
  add_custom_command(
    OUTPUT ${MUSL_UMBRELLA_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${MUSL_BUILD}/lib/libc.so
    COMMAND ${CMAKE_COMMAND} -E env make -C ${MUSL_BUILD} lib/libc.so
            "EXTRA_OBJS=${MACH_EXPORT_OBJS} -Wl,--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/uros_umbrella_mach.list -Wl,--whole-archive $<TARGET_FILE:libposix-uros-pic> -Wl,--no-whole-archive -Wl,--start-group $<TARGET_FILE:libvfs-pic> $<TARGET_FILE:libmach_core-pic> $<TARGET_FILE:libnetname-pic> $<TARGET_FILE:libcap-pic> -Wl,--end-group"
    COMMAND ${CMAKE_COMMAND} -E copy ${MUSL_BUILD}/lib/libc.so ${MUSL_SO}
    COMMAND ${CMAKE_COMMAND} -E touch ${MUSL_UMBRELLA_STAMP}
    DEPENDS musl_extproj
            libposix-uros-pic libvfs-pic libmach_core-pic
            libmach_export_objs
            libnetname-pic libcap-pic
            ${CMAKE_CURRENT_SOURCE_DIR}/uros_umbrella_mach.list
    COMMENT "Linking umbrella libc.so (musl + Uros PIC backend: posix-uros/vfs/mach/netname/cap)"
    VERBATIM
  )
  add_custom_target(musl_umbrella ALL DEPENDS ${MUSL_UMBRELLA_STAMP})

  # Smoke test target: build a standalone hello.c against libc-musl.a.
  # Invoked manually via `ninja musl_smoke`.  Not added to ALL.
  # libgcc is needed for compiler builtins (__udivmoddi4 etc.) that musl
  # leaves to the runtime.  -print-libgcc-file-name is the portable way
  # to locate the matching multilib .a.
  execute_process(
    COMMAND cc -m32 -print-libgcc-file-name
    OUTPUT_VARIABLE MUSL_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  # Phase 1 host smoke (kept).  Links musl against host libgcc only.
  add_custom_target(musl_smoke
    COMMAND cc -m32 -nostdinc
              -I${MUSL_PREFIX}/include
              -nostdlib -static
              ${MUSL_PREFIX}/lib/crt1.o
              ${MUSL_PREFIX}/lib/crti.o
              ${CMAKE_CURRENT_SOURCE_DIR}/musl_smoke.c
              ${MUSL_OUT}
              ${MUSL_LIBGCC}
              ${MUSL_PREFIX}/lib/crtn.o
              -o ${CMAKE_CURRENT_BINARY_DIR}/musl_smoke
    DEPENDS musl_extproj ${CMAKE_CURRENT_SOURCE_DIR}/musl_smoke.c
    COMMENT "Linking musl smoke test (standalone hello against libc-musl.a)"
    VERBATIM
  )

  # Phase 2 link smoke (#250): musl + libposix-uros + hello.c -> i386 ELF.
  # Validates only that the patched syscall_arch.h links cleanly (every
  # __syscallN reference must resolve to libposix-uros's __uros_syscallN).
  # Runtime smoke is deferred to Phase 3, when a real Uros task exists.
  add_custom_target(musl_uros_smoke
    COMMAND cc -m32 -nostdinc
              -I${MUSL_PREFIX}/include
              -nostdlib -static
              ${MUSL_PREFIX}/lib/crt1.o
              ${MUSL_PREFIX}/lib/crti.o
              ${CMAKE_CURRENT_SOURCE_DIR}/musl_smoke.c
              $<TARGET_FILE:libposix-uros>
              ${MUSL_OUT}
              $<TARGET_FILE:libposix-uros>
              ${MUSL_LIBGCC}
              ${MUSL_PREFIX}/lib/crtn.o
              -o ${CMAKE_CURRENT_BINARY_DIR}/musl_uros_smoke
    DEPENDS musl_extproj libposix-uros
            ${CMAKE_CURRENT_SOURCE_DIR}/musl_smoke.c
    COMMENT "Linking musl+libposix-uros smoke (Phase 2 link-only validation)"
    VERBATIM
  )
endif()
