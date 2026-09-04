cmake_minimum_required(VERSION 3.16)
project(bootstrap_server C)

# ===========================================================================
# MIG: generate server-side stubs for the 4 subsystems
# ===========================================================================

set(MIG_INCLUDE_FLAGS
  -I${UROS_ARCH_INCLUDE}
  -I${CMAKE_SOURCE_DIR}/src/mach_kernel
  -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
)

# Helper macro: run CPP | migcom to produce server .c and .h from a .defs
macro(mig_server_stub DEFS_FILE STEM OUT_DIR)
  set(_defs  "${DEFS_FILE}")
  set(_srvC  "${OUT_DIR}/${STEM}Server.c")
  set(_srvH  "${OUT_DIR}/${STEM}Server.h")
  set(_usrC  "${OUT_DIR}/${STEM}User.c")
  set(_hdr   "${OUT_DIR}/${STEM}.h")
  set(_mig_pp "${OUT_DIR}/${STEM}.mig.i")
  add_custom_command(
    OUTPUT ${_srvC} ${_usrC} ${_hdr} ${_srvH}
    # #471: uapi first, and no pipe -- see cmake/mig.cmake for why both.
    COMMAND ${CMAKE_C_COMPILER} -E -x c -I${UROS_UAPI_DIR} ${MIG_INCLUDE_FLAGS}
            ${_defs} -o ${_mig_pp}
    COMMAND $<TARGET_FILE:migcom>
        # ⚠️ Which machine the stubs are for (#453/#426).  migcom defaults to
        # i386, so a rule without this generates i386 layout in silence.
        ${UROS_MIG_TARGET_ARGS}
        -header ${_hdr}
        -sheader ${_srvH}
        -user   ${_usrC}
        -server ${_srvC}
        ${_mig_pp}
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${_defs}
    COMMENT "MIG: ${STEM}.defs -> ${STEM}Server.c"
    VERBATIM
  )
endmacro()

set(MIG_OUT "${CMAKE_BINARY_DIR}/generated/bootstrap_mig")
file(MAKE_DIRECTORY "${MIG_OUT}")

# 1) bootstrap.defs  (subsystem 999999, ServerPrefix do_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/uapi/mach/bootstrap.defs"
  bootstrap  ${MIG_OUT})

# 2) exc.defs  (subsystem 2400, ServerPrefix catch_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/uapi/mach/exc.defs"
  exc  ${MIG_OUT})

# 3) notify.defs  (subsystem 64, ServerPrefix do_)
mig_server_stub(
  "${UROS_UAPI_DIR}/mach/notify.defs"
  notify  ${MIG_OUT})

# 4) service.defs  (subsystem 400, ServerPrefix do_)
mig_server_stub(
  "${UROS_UAPI_DIR}/servers/service.defs"
  service  ${MIG_OUT})

# 5) gpu_server.defs (subsystem 4000) — bootstrap is the gpu_text_puts
#    client (#198 log_forwarder).  We only need the user stubs but the
#    macro emits both; the server-side .c is left unused.
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/src/servers/gpu_server/gpu_server.defs"
  gpu_server  ${MIG_OUT})

set(MIG_SERVER_SOURCES
  ${MIG_OUT}/bootstrapServer.c
  ${MIG_OUT}/excServer.c
  ${MIG_OUT}/notifyServer.c
  ${MIG_OUT}/serviceServer.c
  ${MIG_OUT}/gpu_serverUser.c
)

# ===========================================================================
# Source files
# ===========================================================================

# Core sources (platform-independent)
set(BOOTSTRAP_SOURCES
  bootstrap.c
  service.c
  load.c
  strfcns.c
  # ⚠️ device_read_overwrite.c was here, and is gone rather than moved: it was
  # a private copy of a libmach call, written because the mig_device_read
  # fallback the real wrapper needs was missing from this build.  It is not
  # missing any more -- libmach/syscall_wrappers.c supplies it, calling
  # device_read() which is the same stub under the name this build gives it --
  # and the copy here skipped the trap on the way, which is right for
  # bootstrap's own BDS handle and wrong for a kernel device.  ext_server had
  # the identical file, byte for byte.
  # Issue #186: stage-1 multiboot bundle parser (early-stage server
  # binaries pre-loaded into RAM, removing the need for kernel IDE).
  bundle.c
  # Issue #198: forward bootstrap log output to gpu_server's userspace
  # text path (replaces the kernel VGA console once #199 lands).
  log_forwarder.c
)

# Executable format loaders
# i386: a_out + elf (formats.c selects them; coff/rose/som are for other archs)
# ⚠️ a_out.c only on i386: there is no 64-bit a.out, so x86_64/formats.c names
# elf_switch alone and this file would define a loader nothing points at.
if(UROS_TARGET_ARCH STREQUAL "i386")
  list(APPEND BOOTSTRAP_SOURCES a_out.c)
endif()
list(APPEND BOOTSTRAP_SOURCES elf.c)

# The parser is shared; the rest is the machine's own -- which formats it
# loads, where a task starts, and what it can do with an image besides run it.
list(APPEND BOOTSTRAP_SOURCES parse_args.c)
if(UROS_TARGET_ARCH STREQUAL "i386")
  list(APPEND BOOTSTRAP_SOURCES
    i386/boot_dep.c
    i386/set_regs.c
    i386/formats.c
  )
else()
  list(APPEND BOOTSTRAP_SOURCES
    x86_64/boot_dep.c
    x86_64/set_regs.c
    x86_64/formats.c
  )
endif()

# ===========================================================================
# Build target: Mach standalone executable
# ===========================================================================

# Stubs for symbols normally provided by host libc (-nostdlib build)

add_executable(bootstrap_server
  ${BOOTSTRAP_SOURCES}
  ${MIG_SERVER_SOURCES}
)

add_dependencies(bootstrap_server mig_mach_public_headers)

target_compile_options(bootstrap_server PRIVATE
  -std=gnu11 -fno-builtin  -mstack-protector-guard=global -include stdint.h ${UROS_ARCH_FLAGS} -msse -msse2
)

target_compile_definitions(bootstrap_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(bootstrap_server PRIVATE
  # Local sources first (bootstrap.h, exec.h, elf.h)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # MIG generated output (bootstrapServer.h, excServer.h, etc.)
  ${MIG_OUT}
  # Generated includes (mach/machine -> i386 symlink created in src/CMakeLists.txt)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, servers/)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  # ⚠️ The machine's half of libpthreads, chosen rather than named: AT386 was
  # hardcoded here, and on this target it would have shadowed x86_64's
  # pthread_machdep.h with i386's -- a lock type of the wrong width and a
  # stack-switch sequence in 32-bit assembly.
  ${UROS_LIBPTHREADS_MD}
  # Filesystem headers (file_system.h, externs.h)
  ${CMAKE_SOURCE_DIR}/src/file_systems
  # Kernel headers (mach/, device/, ddb/, i386/) — bootstrap needs kernel-internal
  # extensions (boot_info.h multi-segment support) so kernel must come BEFORE export
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
  # Issue #185: stage-2 reads via BDS (libblk) gated by Uros caps (libcap)
  ${CMAKE_SOURCE_DIR}/src/lib/libblk
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  # libgpu_console (#199 prep) — printf mirror to gpu_server
  ${CMAKE_SOURCE_DIR}/src/lib/libgpu_console
)

# Link against Mach userspace libraries.
# Use --start-group/--end-group to resolve circular dependencies between
# the Mach libraries (libpthreads -> libmach for MIG stubs, libmach ->
# libpthreads for mig_support).
target_link_libraries(bootstrap_server PRIVATE
  -Wl,--start-group
  libsa_fs
  libblk
  libcap
  libgpu_console
  libnetname
  libelf
  libpthreads
  libmach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc
target_link_options(bootstrap_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
)

set_target_properties(bootstrap_server PROPERTIES
  OUTPUT_NAME "bootstrap"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)

# ===========================================================================
# The stage-1 bundle, built by the build (#422)
# ===========================================================================
#
# The bundle is what turns this binary into init: the kernel maps it into the
# bootstrap task and says where on argv, and bootstrap reads bootstrap.conf out
# of it and starts what that names.  Without one there is nothing to start,
# which is what "No bootstrap code loaded with the kernel" has been saying on
# this target since the beginning.
#
# ⚠️ Made here and not by scripts/make-bundle.sh, and that is not a rewrite of
# it.  That script describes an i386 system -- twenty servers, two disk stages,
# a bench selector, driver plug-ins -- and every one of those things is a
# REQUIRED_FILE it checks for.  On this target none of them exists yet.  What
# does exist is one server, so the bundle is two entries; and a rule that knows
# when either of them changed is worth more than a script somebody has to
# remember to re-run after a rebuild.  mkbundle is the piece that is shared,
# which is the piece that was worth sharing.
# #422: -DABLATE_422_RELOC=1 walks the relocation table and writes nothing,
# so a PIE image in the bundle must stop working.  Off in every build.
option(UROS_ABLATE_422_RELOC "Verify the PIE relocations by not applying them" OFF)
if(UROS_ABLATE_422_RELOC)
  target_compile_definitions(bootstrap_server PRIVATE ABLATE_422_RELOC=1)
endif()

if(NOT UROS_TARGET_ARCH STREQUAL "i386")
  set(_BUNDLE ${CMAKE_BINARY_DIR}/bootstrap.bundle)
  set(_CONF   ${CMAKE_CURRENT_BINARY_DIR}/bootstrap.conf)
  set(_SBIN   ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin)
  set(_MOD    ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/modules)

  # The lines that are NOT here are the point: i386's bootstrap.conf names
  # hal_server, block_device_server, default_pager, ext_server and a dozen
  # more, and bootstrap would go looking for each of them in turn.  It names
  # what has crossed: the name server, the task that asks the kernel for a
  # fault (#467), the task that takes its own threads out of calls they will
  # not finish (#475), the task that forks with copy-on-write (#407), and --
  # since #495 -- the first server of the fleet plus the only program on
  # either target that asks it for a capability and checks the answer.
  #
  # ⚠️ The ORDER is load-bearing rather than tidy, and it is least-damage
  # first.  fault_test faults on purpose and act_test kills a thread on
  # purpose; either one, on a kernel that mishandles it, takes the boot down
  # where it stands.  With the name server first, a failure in either can be
  # read as a failure in that one -- put either ahead of it and a #422 result
  # would look like a regression in whatever ran first.
  #
  # netname_test is SECOND, immediately after the server it talks to (#426).
  # It is harmless -- it sends two messages and reads the answers -- so the
  # least-damage rule puts it early; and being right after the name server
  # means that if it fails, the two things that could be at fault are adjacent
  # in the log rather than separated by three programs that fault and kill
  # threads on purpose.
  #
  # irq_claim_test is THIRD, beside netname_test and by the same rule: it only
  # makes RPCs and reads the answers, so it belongs at the harmless end (#457).
  # ⚠️ It is the one entry here whose failure mode is not a wrong line but a
  # MISSING one -- on a kernel where giving an interrupt back halts, nothing
  # after its first line is ever printed, including every entry below it.  That
  # is why it is early rather than last: a halt there is unmistakable, where the
  # same halt at the end would look like a run that simply finished.
  #
  # dma_reclaim_test appears TWICE and consecutively, and where the pair sits
  # is decided from both ends (#513).
  #
  # 🔴 AFTER hal_server, because the first of the two has to die as a DRIVER:
  # it registers, claims the host bridge and reports it bound, so that the
  # second can find the device given back.  Ahead of the HAL there would be
  # nothing to register with.  It needs cap_server too -- claiming a device
  # means presenting a capability for its class -- and that is further up
  # still.
  #
  # 🔴 AND BEFORE block_device_server, because the first of the two also exits
  # holding the WHOLE DMA region table.  Behind the block server it would be
  # taking slots from the one task on this target that moves bytes with a
  # device.
  #
  # 🔴 BOTH LINES CARRY `-w', AND THAT FLAG IS THE EXPERIMENT.  bootstrap
  # blocks on a `-w' entry until it sends bootstrap_completed() or DIES, so:
  # the checker is not created until the hog is gone, which is what makes the
  # reclaim observable at all; and nothing else in the bundle runs while
  # either of them holds the table, which is what keeps cap_test's own DMA
  # arms and the block server's buffers out of the measurement.
  #
  # 🔑 It is one binary named twice rather than two programs, because the two
  # roles must fill the table by exactly the same code: a checker that
  # allocated differently from the hog would be comparing two measurements
  # instead of taking one twice.  argv[1] picks the role.
  #
  # ⚠️ It has a manifest of its own, and it says the opposite of cap_test's on
  # purpose: that one declares no PCI device so its arm [13] reads back a
  # refusal, and this one declares the host bridge's class so the claim can
  # actually happen.  A mechanism is only shown to be a policy when the same
  # file can say yes and no.
  #
  # pthread_test is FOURTH, and it is the biggest program here by an order of
  # magnitude (#425).  It goes after the two that only send messages and before
  # the three that fault, kill threads and fork on purpose: it does nothing
  # deliberately fatal, so a boot that ends inside it is a finding about the
  # thread library rather than collateral from a neighbour.
  #
  # cow_test is last, and by the strongest form of that same rule rather than
  # by seniority: it is the only entry that asks the kernel for a mechanism no
  # boot on this target has ever used, so it is the one most likely to end the
  # run.  Last means every other verdict is already in the log when it does,
  # and the blame cannot land anywhere else.
  #
  # ⚠️ IT WAS MOVED TO THE BOOT DISK AND MOVED BACK (#520, #521).  Loading it
  # through the block device server works -- it was seen to load from
  # /mach_servers and run every one of its arms -- but not reliably: when the
  # load overlaps cap_test's own use of that server, a task faults with no
  # exception server and the boot degrades.  Two runs each way, and the
  # difference was three extra reads' worth of delay.
  #
  # A default that fails one time in two is worse than one that does not
  # exercise a path: a red line nobody can rely on is how a suite stops being
  # read.  So the disk is built and attached on every boot -- the block device
  # server probes it, finds the partition and publishes it, which is more
  # coverage than the nothing it had -- and the load moves back here until #521
  # is answered.
  #
  # cap_server (#495) is THIRD, and it is the first entry here that is a
  # server rather than a program that finishes: it is placed right after the
  # test that just proved the name service answers, because registering with
  # that service is the whole of its startup.
  #
  # cap_test is placed LATE and not next to its server, against the habit that
  # put netname_test next to the name server.  It waits for cap_server itself
  # -- wait_for_cap_server() spins on netname_look_up with no upper bound --
  # so adjacency buys nothing, and it deliberately hands the kernel a message
  # that must be refused halfway (#442).  That is behaviour from the fault_test
  # end of this list, not the netname_test end, so it is ranked by what it does
  # to the kernel rather than by who it talks to.
  #
  # ⚠️ No `-w' on the name server here: bootstrap waits for it by itself
  # (#492).  The decision lives in parse_config_file(), because this file is
  # one of FOUR that write that same line and a flag remembered in four
  # places is a flag that will be missing from the fifth.
  #
  # ⚠️ And no `-w' on cap_server either, for the opposite reason: the flag
  # exists and cap_server does send bootstrap_completed(), so it would work --
  # and it would be a guard that can never fire, because the only task that
  # needs cap_server up already blocks until it is.  A second guard over the
  # same race hides which one is holding.
  #
  # block_device_server is LAST, and after hal_server rather than merely at the
  # end (#520).  Two rules point the same way for once:
  #
  #   - it needs the HAL.  It looks "hal" up in the name server and calls
  #     hal_register_driver, so ahead of hal_server it would find nothing.
  #
  #   - it is the entry most likely to end the run, which is where cow_test's
  #     rule puts a thing.  It is the first task on this target that touches a
  #     device with DMA: it loads a plug-in, probes PCI, allocates wired pages,
  #     programs a controller and takes an interrupt.  Last means every other
  #     verdict is already in the log when it does.
  #
  # ⚠️ It EXITS when it finds no partitions, and that is correct rather than a
  # failure -- there is nothing to serve.  What it must not do is exit quietly:
  # see the note in block_server.c on #400.
  file(GENERATE OUTPUT ${_CONF}
       CONTENT "name_server name_server\nnetname_test netname_test\nirq_claim_test irq_claim_test\ncap_server cap_server\npthread_test pthread_test\ncap_test cap_test\ndl_test dl_test\nfault_test fault_test\nact_test act_test\ncow_test cow_test\nhal_server hal_server\n-w dma_reclaim_test dma_reclaim_test holder\n-w dma_reclaim_test dma_reclaim_test check\nblock_device_server block_device_server\n")

  # ⚠️ It was briefly six while #476 was being hunted -- three copies of the
  # same image under three names, to multiply the STARTING of a task without
  # adding a second program's behaviour to the measurement.  That was an
  # experiment and is not a configuration: it belongs in the issue and in
  # ~/uros-tests, not in what the build ships.
  # ── The first policy this build has ever shipped (#432) ─────────────
  #
  # 🔴 A .cmf IS WHAT TURNS cap_server FROM A MECHANISM INTO A POLICY, and
  # this target shipped none.  i386 has compiled and shipped manifests for a
  # while -- cpustat, ush, hello_exec, dlopen_test, virtual_terminal_server,
  # through scripts/make-bundle.sh -- so cap_server provisions from them on
  # every i386 boot.  x86-64 shipped none because mkmanifest was built only
  # under UROS_USERLAND_ARCH_OK: the mechanism was absent from this TARGET,
  # not from the system.
  #
  # ⚠️ Which is a correction.  This was first written as "none was ever
  # compiled", reached by a search that could not have found the answer:
  # `find -name "*.cmf" -not -path "*/build*"' looks for compiled output
  # everywhere except where compiled output goes.
  #
  # ⚠️ The bundle entry name must be "<symtab>.cmf" exactly: bootstrap builds
  # that string from the name in bootstrap.conf and looks it up.  A file
  # shipped under any other name is a policy nobody reads, which is worse than
  # no policy because it looks like one.
  set(_CAPTEST_CMF
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/cap_test.cmf")
  set(_DMARECLAIM_CMF
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/dma_reclaim_test.cmf")

  add_custom_command(
    OUTPUT ${_BUNDLE}
    COMMAND $<TARGET_FILE:mkbundle> -o ${_BUNDLE}
            bootstrap.conf:${_CONF}
            cap_test.cmf:${_CAPTEST_CMF}
            dma_reclaim_test.cmf:${_DMARECLAIM_CMF}
            name_server:${_SBIN}/name_server
            netname_test:${_SBIN}/netname_test
            irq_claim_test:${_SBIN}/irq_claim_test
            # ⚠️ ONE entry for two configuration lines.  bootstrap resolves a
            # line's path against the bundle, and both lines name the same
            # path -- the roles differ by argv, not by image.  A second copy
            # under a second name would make the hog and the checker two
            # programs that merely look alike.
            dma_reclaim_test:${_SBIN}/dma_reclaim_test
            cap_server:${_SBIN}/cap_server
            pthread_test:${_SBIN}/pthread_test
            cap_test:${_SBIN}/cap_test
            dl_test:${_SBIN}/dl_test
            fault_test:${_SBIN}/fault_test
            act_test:${_SBIN}/act_test
            cow_test:${_SBIN}/cow_test
            hal_server:${_SBIN}/hal_server
            modules/hal/pci_scan.so:${_MOD}/hal/pci_scan.so
            block_device_server:${_SBIN}/block_device_server
            # ⚠️ BOTH drivers now, and ahci.so is here for #432 rather than
            # for its own sake.  It was left out under #520 because
            # modload_load_class() loads whatever is in the class and that
            # issue's staging said one driver at a time.
            #
            # 🔴 What brought it back is a fact about QEMU, not a change of
            # mind: `virtio-blk-pci' is a TRANSITIONAL device, and
            # `iommu_platform=on' -- the property that makes a virtio device
            # honour the IOMMU at all -- is refused on anything but a
            # modern-only one.  QEMU says so on the command line:
            #
            #   VIRTIO_F_IOMMU_PLATFORM was supported by neither legacy nor
            #   transitional device
            #
            # Our virtio driver is the legacy interface -- I/O port BAR0,
            # config at 0x14 -- so it cannot talk to a modern-only device and
            # therefore cannot be placed behind the IOMMU.  #432 needs a
            # driver whose DMA the hardware actually polices, and AHCI is an
            # ordinary PCI bus master with no opt-in to negotiate.
            #
            # 🔑 Which is what #432's own staging said from the start: "one
            # driver at a time, starting with AHCI".
            modules/block/virtio_blk.so:${_MOD}/block/virtio_blk.so
            modules/block/ahci.so:${_MOD}/block/ahci.so
    # 🔥 THE FILE AND NOT THE TARGET.  `DEPENDS cap_test_manifest' is an
    # ORDERING dependency: ninja runs that target first and never notices that
    # its output changed, so the bundle went on shipping a stale manifest while
    # the export tree held the new one.  The symptom was a policy denial for a
    # line the source file plainly contained.
    DEPENDS mkbundle ${_CAPTEST_CMF} ${_DMARECLAIM_CMF}
            name_server_bin netname_test_bin irq_claim_test_bin
            dma_reclaim_test_server cap_server_bin pthread_test_server
            cap_test_server dl_test_bin fault_test_bin act_test_bin cow_test_bin hal_server hal_pci_scan_module
            block_device_server virtio_blk_module ahci_module
            ${_CONF}
    COMMENT "Bundle: bootstrap.conf + 12 tasks + hal_server + block_device_server"
    VERBATIM)

  add_custom_target(bootstrap_bundle ALL DEPENDS ${_BUNDLE})
endif()
