# MIG stub generation, shared by both targets.
#
# #416.  These two rules used to live inside the i386 half of
# src/mach_kernel/CMakeLists.txt -- below the `return()` that ends the x86-64
# branch -- with -I i386 and -I i386/AT386 written into them.  So they were
# i386 rules by position and by content, and the new target had no way to run
# mig at all.  That is why nine machine-independent sources still fail on
# x86-64: they include a generated *_server.h that has never been generated.
#
# The include path is now the caller's, in ${UROS_MIG_INCLUDES}.  Nothing else
# changed; the regression test is that the i386 output stays byte-identical.
#
# ⚠️ migcom runs on the host and generates for the target.  Any sizeof of a
# host type used to reason about a target type is a bug, and a silent one --
# see the note above the assertion emitter in src/lib/migcom/utils.c, which is
# what makes migcom's beliefs about message layout checkable by the compiler
# that will lay the message out.

# Helper function to add MIG generation rule
# Use -x c to force C preprocessing of .defs files (modern GCC ignores unknown extensions)
function(add_mig_server DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
    set(SERVER_C ${OUTPUT_DIR}/${DEFS_NAME}_server.c)
    set(SERVER_H ${OUTPUT_DIR}/${DEFS_NAME}_server.h)
    
    add_custom_command(
        OUTPUT ${SERVER_C} ${SERVER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -x c
                ${UROS_MIG_INCLUDES}
                ${KERNEL_DEFINES}
                ${DEFS_FILE} | 
                $<TARGET_FILE:migcom>
                -sheader ${SERVER_H}
                -server ${SERVER_C}
                -header /dev/null
                -user /dev/null
        DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEFS_FILE}
        COMMENT "MIG: ${DEFS_NAME} (server)"
        VERBATIM
    )
    
    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED} ${SERVER_C} PARENT_SCOPE)
endfunction()

function(add_mig_user DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
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
                ${UROS_MIG_INCLUDES}
                ${KERNEL_DEFINES}
                -DKERNEL_USER=1
                -UKERNEL_SERVER
                ${DEFS_FILE} |
                $<TARGET_FILE:migcom>
                -header ${USER_H}
                -user ${USER_C}
                -server /dev/null
        DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEFS_FILE}
        COMMENT "MIG: ${DEFS_NAME} (user)"
        VERBATIM
    )
    
    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED} ${USER_C} PARENT_SCOPE)
endfunction()
