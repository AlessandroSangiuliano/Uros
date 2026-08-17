# MIG stub generation, shared by both targets.
#
# #416.  These two rules used to live inside the i386 half of
# src/mach_kernel/CMakeLists.txt -- below the `return()` that ends the x86-64
# branch -- with -I i386 and -I i386/AT386 written into them.  So they were
# i386 rules by position and by content, and the new target had no way to run
# mig at all.  That is why nine machine-independent sources still fail on
# x86-64: they include a generated *_server.h that has never been generated.
#
# ⚠️ And so is the architecture the .defs themselves are preprocessed for, in
# ${UROS_MIG_DEFS_ARCH}.  The preprocessing runs with the HOST compiler and no
# -m32/-m64 -- it is producing text for migcom, not code -- so __i386__ and
# __x86_64__ inside a .defs describe the machine doing the build.  A type
# whose size follows the target's pointer width cannot be written against
# them, and writing it against them looks right on whichever machine you are
# sitting at (#453).
#
# That variable carries TWO macros -- MIG_ARCH_<NAME> and MIG_PTR_BITS -- and
# the top-level CMakeLists says at length which question each one answers.  It
# is set there and nowhere else: it used to be set once per architecture inside
# the kernel's directory, which is how the width never reached the .defs the
# userland libraries generate (#422).
#
# ⚠️ So is the target, in ${UROS_MIG_TARGET_ARGS}, and it must be set: migcom
# defaults to i386 for compatibility with every invocation that predates the
# option, so a rule that forgets the flag silently generates i386 layout.  That
# is not hypothetical -- every x86-64 stub generated before #453 carried i386's
# 12-byte port descriptor, and nobody found out because none of them had yet
# been compiled.  The generated _Static_asserts caught it the moment one was.
#
# The include path is now the caller's, in ${UROS_MIG_INCLUDES}.  Nothing else
# changed; the regression test is that the i386 output stays byte-identical.
#
# ⚠️ migcom runs on the host and generates for the target.  Any sizeof of a
# host type used to reason about a target type is a bug, and a silent one --
# see the note above the assertion emitter in src/lib/migcom/utils.c, which is
# what makes migcom's beliefs about message layout checkable by the compiler
# that will lay the message out.

# 🔥 TWO COMMANDS, NOT A PIPE (#471).
#
# These rules used to read `cc -E ... ${DEFS_FILE} | migcom ...', and a shell
# gives a pipeline the exit status of its LAST command.  So a preprocessor
# that died -- on a missing include, say -- left migcom parsing a truncated
# stream, which it did without complaint, and the build saw zero.  The stub
# that came out was missing routines: service_checkin absent from
# service_user.c, every _X absent from device_pager_server.c, no diagnostic
# anywhere.  A client would have found out at link time; a server would have
# found out by not answering a message id it believed it implemented.
#
# The preprocessed text goes to ${MIG_PP} and migcom is handed the NAME.
# CMake stops a custom command at the first COMMAND that fails, so cc's exit
# status is now the rule's.  `set -o pipefail' would have done it on this
# machine and not in POSIX sh, and would have had to be remembered at each
# call site; this cannot be forgotten because there is no pipe left to forget
# about.
#
# Use -x c to force C preprocessing of .defs files (modern GCC ignores unknown extensions)
function(add_mig_server DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
    set(MIG_PP ${OUTPUT_DIR}/${DEFS_NAME}_server.mig.i)
    set(SERVER_C ${OUTPUT_DIR}/${DEFS_NAME}_server.c)
    set(SERVER_H ${OUTPUT_DIR}/${DEFS_NAME}_server.h)
    
    add_custom_command(
        OUTPUT ${SERVER_C} ${SERVER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -x c
                -I${UROS_UAPI_DIR}
                ${UROS_MIG_INCLUDES}
                ${KERNEL_DEFINES}
                ${UROS_MIG_DEFS_ARCH}
                ${DEFS_FILE} -o ${MIG_PP}
        COMMAND $<TARGET_FILE:migcom>
                ${UROS_MIG_TARGET_ARGS}
                -sheader ${SERVER_H}
                -server ${SERVER_C}
                -header /dev/null
                -user /dev/null
                ${MIG_PP}
        DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEFS_FILE}
        COMMENT "MIG: ${DEFS_NAME} (server)"
        VERBATIM
    )
    
    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED} ${SERVER_C} PARENT_SCOPE)
endfunction()

function(add_mig_user DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
    set(MIG_PP ${OUTPUT_DIR}/${DEFS_NAME}_user.mig.i)
    set(USER_C ${OUTPUT_DIR}/${DEFS_NAME}_user.c)
    # Use _user.h suffix to avoid conflicts with source headers (e.g. mach/memory_object.h)
    set(USER_H ${OUTPUT_DIR}/${DEFS_NAME}_user.h)
    
    # For KernelUser stubs: define KERNEL_USER=1 so .defs files enable
    # the KernelUser subsystem modifier (generates mach_msg_send_from_kernel
    # instead of mach_msg_overwrite). Undefine KERNEL_SERVER to prevent
    # memory_object.defs (which has both guards) from also setting
    # IsKernelServer, which would emit '#undef MACH_KERNEL' and cause
    # type conflicts between user-space and kernel type definitions.
    add_custom_command(
        OUTPUT ${USER_C} ${USER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -x c
                -I${UROS_UAPI_DIR}
                ${UROS_MIG_INCLUDES}
                ${KERNEL_DEFINES}
                ${UROS_MIG_DEFS_ARCH}
                -DKERNEL_USER=1
                -UKERNEL_SERVER
                ${DEFS_FILE} -o ${MIG_PP}
        COMMAND $<TARGET_FILE:migcom>
                ${UROS_MIG_TARGET_ARGS}
                -header ${USER_H}
                -user ${USER_C}
                -server /dev/null
                ${MIG_PP}
        DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEFS_FILE}
        COMMENT "MIG: ${DEFS_NAME} (user)"
        VERBATIM
    )
    
    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED} ${USER_C} PARENT_SCOPE)
endfunction()

# ── Userland stubs (#426) ────────────────────────────────────────────────
#
# The third kind, and the one libmach is made of: a task calling the kernel,
# so neither KERNEL_SERVER nor KERNEL_USER is defined.  Both halves come out
# of one run -- the user stubs a client links, and the server stubs a server
# demultiplexes with -- because migcom writes them from one parse.
#
# ⚠️ i386 does NOT come through here.  It generates its userland stubs from
# src/lib/CMakeLists.txt via the scripts/mig wrapper, which never passes
# -target: harmless there, because migcom's default IS i386.  Adding a target
# flag to that wrapper is not a one-line change -- its argument loop routes
# any flag it does not recognise to the C PREPROCESSOR, so `-target x86_64'
# would arrive at cc and the architecture would never reach migcom at all.
#
# So this calls migcom directly, the way add_mig_server and add_mig_user
# above already do, and inherits their target handling rather than teaching a
# 1990 shell script a new option.  When i386's userland moves here too, that
# wrapper can go.
#
# ⚠️ The target is spelled out here rather than taken from
# ${UROS_MIG_TARGET_ARGS}, and that is now only a habit: when this function was
# written that variable was set inside the kernel's directory scope and was
# empty everywhere else, which is exactly the silent i386 layout the note at
# the top of this file is about.  It is set at the top level today, so the two
# say the same thing -- left as it is because it is one expression either way
# and this one cannot be emptied by a scope.
function(add_mig_userland DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
    set(MIG_PP ${OUTPUT_DIR}/${DEFS_NAME}.mig.i)
    set(USER_C   ${OUTPUT_DIR}/${DEFS_NAME}_user.c)
    set(SERVER_C ${OUTPUT_DIR}/${DEFS_NAME}_server.c)
    set(USER_H   ${OUTPUT_DIR}/${DEFS_NAME}.h)
    set(SERVER_H ${OUTPUT_DIR}/${DEFS_NAME}_server.h)

    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    set_source_files_properties(${USER_C} ${SERVER_C} ${USER_H} ${SERVER_H}
                                PROPERTIES GENERATED TRUE)

    add_custom_command(
        OUTPUT ${USER_C} ${SERVER_C} ${USER_H} ${SERVER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -x c
                -I${UROS_UAPI_DIR}
                ${UROS_MIG_USERLAND_INCLUDES}
                ${UROS_MIG_DEFS_ARCH}
                ${DEFS_FILE} -o ${MIG_PP}
        COMMAND $<TARGET_FILE:migcom>
                -target ${UROS_TARGET_ARCH}
                -header ${USER_H}
                -sheader ${SERVER_H}
                -user ${USER_C}
                -server ${SERVER_C}
                ${MIG_PP}
        # ⚠️ ${UROS_MIG_COMMON_DEFS} and not just the .defs named here.  A
        # .defs `import's others -- mach_types.defs, std_types.defs -- and a
        # rule that depends only on the file it is given does not rebuild
        # when a TYPE changes underneath it.  That is not theoretical: the
        # security_token_t fix of #470 was made and the stubs kept their old
        # layout, so the build went on failing an assertion about a file
        # that had just been corrected.
        DEPENDS migcom ${DEFS_FILE} ${UROS_MIG_COMMON_DEFS}
        COMMENT "MIG: ${DEFS_NAME} (userland, ${UROS_TARGET_ARCH})"
        VERBATIM
    )

    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED}
        ${USER_C} ${SERVER_C} PARENT_SCOPE)
endfunction()
