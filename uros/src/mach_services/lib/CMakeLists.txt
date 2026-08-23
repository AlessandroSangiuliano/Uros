cmake_minimum_required(VERSION 3.16)

# uros/src/mach_services/lib — the genuinely inherited OSF libraries (#396).
#
# The actively-developed Uros libraries (and migcom + the MIG helper
# functions) live in src/lib, which src/CMakeLists adds BEFORE this
# directory — so add_mig_defs() and the maybe_add macro are already
# defined by the time this file runs.

# Same -fPIC rationale as src/lib: these statics link into PIE servers.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

maybe_add(libmach_p)
maybe_add(libmach_maxonstack)
# libsa_mach merged into libmach (#106)
# libmachid: built, installed, and linked by nothing — the six mentions of
# libmachid.a in build.ninja are all its own build, install and phony rules.
# It also never carried UROS_ARCH_FLAGS, so it was compiled at the host's
# width and archived into export/.../i386/user/lib as if it were target code,
# holding 32-bit Mach types the whole time.  Nobody noticed because nobody
# links it; #453's <mach/i386/vm_types.h> guard is what surfaced it.
#
# The sources stay in the tree and the arch flags are now in its own
# CMakeLists, so uncommenting is all it takes if something ever needs the
# Mach ID service.  Verdict belongs to #433, the heritage-husk inventory.
#maybe_add(libmachid)
# libcthreads / librthreads: NOT BUILT (#434).
#
# Both are deprecated -- the live path is libpthreads plus libmach's crt0 --
# and nothing links either of them: no target_link_libraries in the tree
# names them.  They were still compiled, and that cost more than it sounds:
#
#   - librthreads wrote, at CONFIGURE time and unconditionally, a
#     `generated/include/stdlib.h' forwarding to <sa_mach/stdlib.h> into a
#     directory forty-eight targets share with musl.  That wrapper is what
#     turned a missing musl header into `conflicting types for time_t' and
#     made a clean build stop.  Not building librthreads removes the writer
#     rather than conditioning it.
#
#   - libcthreads includes <sa_mach/stdio.h> directly, whose off_t is an
#     `unsigned int' against musl's.  It only compiled because it happened
#     to run before musl was installed; once the ordering above was made
#     real, the conflict was no longer avoidable.  Fixing it would mean
#     reconciling sa_mach's types with musl's inside a library nobody uses.
#
# The sources stay in the tree as study material -- this is the build list,
# not a deletion.
#
# ── #425's verdict: RETIRED, not ported ─────────────────────────────────
#
# That issue asked for one of the two, in writing, and this is it.  They are
# NOT being ported to x86-64, and the reason is the one its own body gives:
# porting two obsolete thread packages to a new architecture to keep them
# compiling is work with no consumer.  libpthreads is the 1:1 POSIX
# implementation (#82) and is what everything uses -- pthread_test passes 23
# of 23 on x86-64 through it, which is the evidence that the live path needs
# nothing from these two.
#
# 🔑 Recorded here rather than left as an omission, because a package that is
# simply never mentioned again reads as forgotten, and the next person to
# notice `librthreads/AT386/lock.S' has no x86-64 sibling cannot tell whether
# that is a decision or a gap.  It is a decision, taken 12/08.
#
# ⚠️ A stale artefact outlives the rule: builds from before #434 left
# `export/uros/i386/user/lib/librthreads.a' behind, and nothing rebuilds or
# removes it.  Anything that linked -lrthreads would silently pick up a
# month-old library rather than fail.  Deleted; if one reappears, it means
# something turned these back on.
#
# What is left of them belongs to #433, the heritage-husk inventory: whether
# the sources stay as study material or go.
#maybe_add(libcthreads)
#maybe_add(libcthreads_p)
#maybe_add(librthreads)
#maybe_add(libxmm)  # excluded for now: needs K&R modernization
maybe_add(libservice)
maybe_add(libnetmemory)
