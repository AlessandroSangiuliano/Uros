#!/usr/bin/env python3
"""What MACH_ASSERT actually switches, counted from the source (#485).

Both targets build Release with MACH_ASSERT on.  #482 found out what that can
cost: vm_page_grab() was mapping every page it took off the free list, reading
all 1024 words to check a poison pattern, and unmapping it -- 44% of a
copy-on-write fault, for a hunt (#385) that closed months ago.  Gating that one
site took the fault from 5,790 cycles to 3,390.

The question #485 asks is not whether that site should have been on.  It is
what an assertion is allowed to cost, and which of the things filed under this
switch are assertions at all.  Answering it needs the sites enumerated, and
enumerated MECHANICALLY -- 4,000-odd of them is well past where sampling and
forming an impression is a method.

Three reports, because there are three different questions:

  1. CONFIG    -- is the switch even operable?  It has to be settable from one
                  place before anything else is worth deciding.
  2. BLOCKS    -- the multi-line `#if MACH_ASSERT' regions, ranked by what they
                  look like they cost.  This is where an expensive thing hides,
                  because a one-line assert is a comparison and cannot.
  3. SIDE      -- asserts whose expression CALLS something.  These are the
                  dangerous ones in the other direction: assert(ex) evaluates
                  ex when the switch is on and does not when it is off, so an
                  expression with a side effect makes the two builds behave
                  differently -- and turning the switch off in Release would
                  be the change that removes it.

⚠️ The cost signals are heuristics and are labelled as such.  A tool that
sorted these into "keep" and "delete" would be inventing an answer; this one
sorts them into "read this" and "you may skip this", which is a claim it can
actually support.

Usage:
    scripts/assert-census.py            # all three reports
    scripts/assert-census.py --config
    scripts/assert-census.py --blocks [--all]
    scripts/assert-census.py --side
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "uros", "src", "mach_kernel")
CONFIG_CMAKE = os.path.join(ROOT, "uros", "cmake", "kernel-config.cmake")
KERNEL_CMAKE = os.path.join(KERNEL, "CMakeLists.txt")


def strip_comments(text):
    return "\n".join(line.split("#")[0] for line in text.splitlines())


def cmake_block(text, name):
    """The body of set(NAME ...), delimited by counting parentheses.

    ⚠️ Not by looking for a closing paren at the start of a line.  The first
    version of this did, KERNEL_DEFINES_BARE ends its last entry and its
    parenthesis on one line, and the match ran on into the next set() -- which
    reported 70 defines for a list of 16 and looked entirely plausible.
    """
    m = re.search(r"\bset\(\s*" + name + r"\b", text)
    if not m:
        return ""
    i, depth = m.end(), 1
    while i < len(text) and depth:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    return text[m.end():i - 1]


def ninja_deps(build_dir):
    """What each object actually included, asked of ninja's own database.

    🔴 Not the .d files -- ninja keeps dependencies in a binary .ninja_deps and
    writes no .d at all, so a scan for them returns zero, and zero looks like
    an answer.  It cost one wrong report here before the implausibility of it
    was noticed.
    """
    import subprocess
    d = os.path.join(ROOT, build_dir)
    if not os.path.isdir(d):
        return {}
    try:
        out = subprocess.run(["ninja", "-t", "deps"], cwd=d, text=True,
                             capture_output=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    deps, cur = {}, None
    for line in out.splitlines():
        if not line.startswith(" "):
            m = re.match(r"(\S+):\s+#deps", line)
            cur = m.group(1) if m else None
            if cur:
                deps[cur] = []
        elif cur:
            deps[cur].append(line.strip())
    return deps


def header_reaches(build_dir, macro, header):
    """Does every compiled TU that tests `macro' also see `header'?

    This is what makes dropping a redundant -D a demonstrable change rather
    than a hopeful one: if some translation unit tests the macro without the
    header that defaults it, removing the -D leaves that unit with the macro
    undefined -- `#if UNDEFINED' is 0 -- and switches its code off in silence.
    """
    deps = ninja_deps(build_dir)
    if not deps:
        return None
    rx = re.compile(r"^\s*#\s*if(n?def)?\s+.*\b" + macro + r"\b", re.M)
    base = os.path.join(ROOT, build_dir)
    uses = sees = 0
    blind = []
    for tgt, incs in deps.items():
        if not tgt.endswith(".c.o"):
            continue
        src = next((i for i in incs if i.endswith(".c")), None)
        if not src:
            continue
        path = src if os.path.isabs(src) else os.path.normpath(
            os.path.join(base, src))
        if not os.path.exists(path):
            continue
        try:
            if not rx.search(open(path, errors="replace").read()):
                continue
        except OSError:
            continue
        uses += 1
        if any(header in i for i in incs):
            sees += 1
        else:
            blind.append(os.path.relpath(path, ROOT))
    return uses, sees, blind


def report_config():
    """Is the switch operable from one place?"""
    table = {}
    body = strip_comments(cmake_block(open(CONFIG_CMAKE).read(),
                                      "UROS_KERNEL_CONFIG"))
    for line in body.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[1]] = parts[2]

    cm = open(KERNEL_CMAKE).read()

    def defines(varname, dashed):
        out = {}
        for tok in strip_comments(cmake_block(cm, varname)).split():
            if dashed:
                if not tok.startswith("-D"):
                    continue
                tok = tok[2:]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(=\S+)?", tok):
                continue
            name, _, value = tok.partition("=")
            # A bare -DFOO is 1 to the preprocessor.  Recording it as "1"
            # rather than as "no value" is what stops this reporting three
            # disagreements that are not disagreements.
            out[name] = value if value else "1"
        return out

    print("== 1. CONFIG: where the knobs come from ==")
    print(f"   kernel-config.cmake: {len(table)} knobs.  That file says of "
          "itself:")
    print('     "The values live here ... There is no other place, and that '
          'is the point."')

    per_target = {}
    for label, var, dashed in (("i386", "KERNEL_DEFINES", True),
                               ("x86-64", "KERNEL_DEFINES_BARE", False)):
        d = defines(var, dashed)
        per_target[label] = d
        both = sorted(k for k in d if k in table)
        redundant = [k for k in both if table[k] == d[k]]
        print(f"\n   {label} ({var}): {len(d)} defines, "
              f"{len(both)} of them ALSO in the table, "
              f"{len(redundant)} redundantly")
        for k in both:
            a, b = table[k], d[k]
            # ⚠️ A -D that DIFFERS is not a defect: the generator writes every
            # value under #ifndef precisely so a per-target -D can override the
            # shared default, and says so in as many words.  MACH_KDB is 1 in
            # the table and 0 here on purpose.
            #
            # The one worth flagging is the opposite.  A -D that REPEATS the
            # table value overrides nothing -- it just puts the same knob in
            # two places, and that is what makes it unsettable from either:
            # clearing one leaves the other standing.
            if a == b:
                mark = "   <-- REDUNDANT: two sources, one value"
            else:
                mark = "   (deliberate per-target override)"
            print(f"     {k:<26} table={a:<3} -D={b:<3}{mark}")

    print("\n   Can the redundant -D be dropped?  Only if every compiled unit"
          " that tests")
    print("   the macro also sees the header that defaults it — otherwise"
          " dropping it")
    print("   leaves `#if UNDEFINED\', which is 0, and switches code off in"
          " silence.\n")
    for macro, header in (("MACH_ASSERT", "mach_assert.h"),
                          ("MACH_DEBUG", "mach_debug.h"),
                          ("MACH_HOST", "mach_host.h"),
                          ("MACH_KDB", "mach_kdb.h"),
                          ("STAT_TIME", "stat_time.h"),
                          ("TASK_SWAPPER", "task_swapper.h")):
        for label, bd in (("i386", "uros/build"),
                          ("x86-64", "uros/build-x86_64")):
            r = header_reaches(bd, macro, header)
            if r is None:
                print(f"     {macro:<14} {label:<7} (no build to ask)")
                continue
            uses, sees, blind = r
            if uses == 0:
                verdict = "nothing tests it here"
            elif sees == uses:
                # ⚠️ Two different safeties, and conflating them would be a
                # report that is true in one sense and misleading in the one
                # that matters.  The header reaching every unit says dropping
                # the -D changes no unit's VISIBILITY of the macro; it says
                # nothing about its VALUE.  Where the -D overrides the table,
                # dropping it silently moves the knob.
                dl = per_target.get(label, {})
                if macro in dl and macro in table and dl[macro] != table[macro]:
                    verdict = (f"{sees}/{uses} see <{header}> — but this -D "
                               f"OVERRIDES: dropping it moves the value "
                               f"{dl[macro]} -> {table[macro]}")
                else:
                    verdict = f"{sees}/{uses} see <{header}> — safe to drop"
            else:
                verdict = (f"{sees}/{uses} see <{header}> — NOT SAFE: "
                           + ", ".join(blind[:3]))
            print(f"     {macro:<14} {label:<7} {verdict}")

    print("""
   🔑 The generated <mach_assert.h> is `#ifndef MACH_ASSERT / #define
   MACH_ASSERT 1', so the -D wins where both exist -- and REMOVING the -D
   changes nothing, because the header puts it back.  Whatever #485 decides,
   the switch has to become settable from one place first, or the decision
   cannot be carried out.""")


BLOCK_START = re.compile(r"^\s*#\s*if(n?def)?\s+.*\bMACH_ASSERT\b")
# A guard that names a SECOND switch has already been decided about: somebody
# read it, put it in a category and gave it a name of its own.  A bare
# `#if MACH_ASSERT\' has not.  Keeping the two apart is what turns this report
# from a fixed list into a work queue that shrinks.
BLOCK_TRIAGED = re.compile(r"^\s*#\s*if(n?def)?\s+.*\bMACH_ASSERT\b\s*&&")
ANY_IF = re.compile(r"^\s*#\s*if(n?def)?\b")
ANY_ENDIF = re.compile(r"^\s*#\s*endif\b")

# What makes a block look like it costs something.  Heuristics, and labelled.
SIGNALS = (
    # What KIND of thing it is -- these map onto the three categories #485
    # asks for, and they are the ones worth sorting by.
    #
    #   ddb     only the debugger reaches it: text size, not path cost
    #   panic   it stops the machine: a real invariant
    #   repair  it ASSIGNS to something that outlives the block -- so the two
    #           builds do not merely check differently, they behave
    #           differently, and turning the switch off removes a repair
    ("ddb", re.compile(r"<ddb/|\bdb_[a-z_]+\s*\(")),
    ("panic", re.compile(r"\bpanic\s*\(")),
    ("repair", re.compile(r"^\s*[a-z_][A-Za-z0-9_]*(->|\.)[A-Za-z0-9_>.\-]*\s*=[^=]",
                          re.M)),
    # ... and what it COSTS.
    ("loop", re.compile(r"\b(for|while)\s*\(")),
    ("page", re.compile(r"\bPAGE_SIZE\b|\bkmap\b|\bkunmap\b")),
    ("walk", re.compile(r"\bpv_|\bqueue_|->next\b|->pv_")),
    ("io", re.compile(r"\bprintf\b|\bdevice_(read|write)")),
)


def built_sources():
    """Which .c files the two kernels actually compile, asked of the BUILD.

    🔴 Not guessed from the tree.  Half of what walks out of os.walk() is
    heritage that nothing links -- ppc/, hp_pa/, dipc/, xmm/, scsi/ -- and
    classifying assertions in code that is never compiled is work spent on
    nothing.  The authority is the object files each build produced.

    ⚠️ Falls back to "everything" with a warning if neither build directory
    has been built, rather than quietly reporting a census of the whole tree
    as though it were the census of the kernel.
    """
    import glob
    built = set()
    for bd in ("uros/build", "uros/build-x86_64"):
        d = os.path.join(ROOT, bd, "src", "mach_kernel",
                         "CMakeFiles", "mach_kernel.dir")
        for obj in glob.glob(os.path.join(d, "**", "*.c.o"), recursive=True):
            rel = os.path.relpath(obj, d)[:-2]          # drop the .o
            built.add(os.path.normpath(os.path.join(KERNEL, rel)))
    return built


def sources():
    for base, dirs, files in os.walk(KERNEL):
        dirs[:] = [d for d in dirs if d not in (".git", "conf")]
        for f in sorted(files):
            if f.endswith((".c", ".h")):
                yield os.path.join(base, f)


FUNC_DEF = re.compile(r"^([a-zA-Z_][a-zA-Z0-9_]*)\s*\(")


def enclosing_function(lines, idx):
    """The function a block sits in, by scanning back for a definition.

    🔑 File granularity is not enough to answer the question this is for.
    "Is there another #482 hiding" means "is an expensive block on a path that
    runs often", and vm_resident.c holds both vm_page_grab -- every page
    allocation in the kernel -- and vm_sort_free_list, which runs when somebody
    asks for physically contiguous memory.  The file cannot tell them apart and
    the function name can.

    ⚠️ Heuristic: it looks for an identifier at column zero followed by `(\',
    which is this tree\'s style, and gives up rather than guessing.
    """
    for j in range(idx, max(0, idx - 400), -1):
        line = lines[j]
        if not line or line[0] in " \t#*/":
            continue
        m = FUNC_DEF.match(line)
        if m and m.group(1) not in ("if", "for", "while", "switch", "return"):
            return m.group(1)
        if line.startswith("}"):
            # left the previous function without finding a header
            continue
    return "?"


def report_blocks(show_all=False):
    print("\n== 2. BLOCKS: the multi-line #if MACH_ASSERT regions ==")
    built = built_sources()
    if not built:
        print("   ⚠️ NEITHER BUILD DIRECTORY HAS OBJECTS — cannot tell what is")
        print("      compiled, so everything below is the whole tree, most of")
        print("      which nothing links.  Build first.")
    found = []
    for path in sources():
        try:
            lines = open(path, errors="replace").read().splitlines()
        except OSError:
            continue
        i = 0
        while i < len(lines):
            if BLOCK_START.match(lines[i]):
                depth, j = 1, i + 1
                while j < len(lines) and depth:
                    if ANY_IF.match(lines[j]):
                        depth += 1
                    elif ANY_ENDIF.match(lines[j]):
                        depth -= 1
                    j += 1
                body = "\n".join(lines[i + 1:j - 1])
                # Comments and blank lines are not cost.
                code = [l for l in body.splitlines()
                        if l.strip() and not l.strip().startswith(("*", "/*", "//"))]
                hits = [name for name, rx in SIGNALS if rx.search(body)]
                # A header is counted as built if anything that includes it
                # is -- which cannot be answered from object files, so headers
                # are reported and marked rather than dropped.
                is_built = (path in built) or path.endswith(".h") or not built
                triaged = bool(BLOCK_TRIAGED.match(lines[i]))
                found.append((len(hits), len(code), path, i + 1, hits,
                              is_built and not triaged, triaged,
                              enclosing_function(lines, i)))
                i = j
                continue
            i += 1

    found.sort(key=lambda t: (-t[0], -t[1]))
    triaged = [f for f in found if f[6]]
    live = [f for f in found if f[5]]
    dead = len(found) - len(live) - len(triaged)
    loud = [f for f in live if f[0] or f[1] > 10]
    print(f"   {len(found)} blocks across {len({f[2] for f in found})} files;"
          f" {dead} in sources NOTHING COMPILES,")
    print(f"   {len(triaged)} already triaged onto a switch of their own.")
    print(f"   {len(live)} are left in the kernel that builds, and {len(loud)}"
          " of those carry a")
    print("   cost signal or exceed ten lines of code — those are the ones"
          " still to read.\n")
    for hits, n, path, line, names, _, _t, fn in (live if show_all else loud):
        rel = os.path.relpath(path, ROOT)
        tag = ",".join(names) if names else "-"
        print(f"   {n:>4} lines  [{tag:<16}] {fn}()  {rel}:{line}")
    if not show_all:
        print("\n   (--all to list the quiet ones too)")


ASSERT_CALL = re.compile(r"\bassert(_static)?\s*\(")

# A call whose NAME suggests it changes something rather than answering a
# question.  Heuristic, and the point of it is to turn "399 asserts contain a
# call" -- which nobody will read -- into a shortlist that somebody will.
#
# ⚠️ It errs toward including: a pure predicate that happens to be named
# `clear_something\' costs one reading, while a mutator missed here is a
# behaviour difference between two builds that nobody looks for.
MUTATES = re.compile(r"^(.*_)?(lock|unlock|alloc|free|grab|release|remove|"
                     r"insert|enter|deallocate|destroy|reference|deactivate|"
                     r"activate|wire|unwire|set|clear|add|del|put|take|"
                     r"init|copy|write|zero)(_.*)?$")


def report_side_effects():
    """assert(ex) evaluates ex only when the switch is on."""
    print("\n== 3. SIDE: asserts whose expression calls something ==")
    print("   assert(ex) is `if (!(ex)) Assert(...)' when the switch is on and")
    print("   ((void)0) when it is off, so ex is NOT EVALUATED in a build")
    print("   without it.  An expression that does work rather than asking a")
    print("   question makes the two builds behave differently, and turning")
    print("   the switch off in Release is what would remove it.\n")
    built = built_sources()
    hits, benign = [], 0
    for path in sources():
        if path.endswith("kern/assert.h"):
            continue                      # the macro definition itself
        if built and path not in built and not path.endswith(".h"):
            continue                      # nothing compiles this
        try:
            lines = open(path, errors="replace").read().splitlines()
        except OSError:
            continue
        for n, line in enumerate(lines, 1):
            m = ASSERT_CALL.search(line)
            if not m:
                continue
            # The expression, as far as this line goes.
            expr = line[m.end():]
            # A call inside the expression: an identifier followed by (
            calls = re.findall(r"\b([a-z_][a-z0-9_]{2,})\s*\(", expr)
            calls = [c for c in calls if c not in ("if", "while", "for",
                                                   "sizeof", "return")]
            if not calls:
                continue
            suspect = sorted({c for c in calls if MUTATES.match(c)})
            if suspect:
                hits.append((os.path.relpath(path, ROOT), n, suspect,
                             line.strip()))
            else:
                benign += 1

    print(f"   {len(hits) + benign} asserts in compiled sources call"
          " something.")
    print(f"   {benign} call only things that read; {len(hits)} call something"
          " whose NAME")
    print("   suggests it changes state — those are the ones to read.\n")
    for rel, n, calls, text in hits:
        print(f"   {rel}:{n}   [{', '.join(calls)}]")
        print(f"       {text[:96]}")


def main():
    args = set(sys.argv[1:])
    want_all = "--all" in args
    picks = args & {"--config", "--blocks", "--side"}
    if not picks:
        picks = {"--config", "--blocks", "--side"}
    if "--config" in picks:
        report_config()
    if "--blocks" in picks:
        report_blocks(want_all)
    if "--side" in picks:
        report_side_effects()


if __name__ == "__main__":
    main()
