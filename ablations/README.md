# Ablations

A fix is verified by taking it out: with the fix gone, the test written for it must fail, and fail in the way the defect fails. This directory holds the fixes taken out, one patch each. The kernel holds only the fix and its test.

They used to live in the kernel, as `UROS_ABLATE_*` and `UROS_WIDEN_*` CMake options and `#if` blocks. By the epic at `6e647c11` there were 83 options and 342 places in 56 kernel files. The code grew with every fix, and an ablated branch was compiled only when its option was on, so nothing noticed when one stopped meaning what it once meant.

A patch here can rot too, but not silently: once it no longer applies, `scripts/ablate.sh` stops and says so. That is the moment to look at the test again.

## Two kinds

- **ablation**: brings back the defect. It is usually the reverse of the fix's code and nothing more: no comments, no test, and nothing a test still uses (a counter, for example).
- **widen**: makes a race window wide enough to be met on demand. It is a spin at the point the race needs, plus whatever arms it in the test. It is applied before the ablation, and alone for the fixed arm of an alternated run: the fix must hold with the window wide open.

## The header

Lines before the first `diff --git` are a header; `git apply` skips them.

```
Kind: ablation
Issue: #642
Takes out: what the fix did, in one line
Run: --entry 25 600 -smp 4
Expect: <thread_doswapin\+0x
```

- `Takes out:` (an ablation) or `Holds open:` (a widen patch) says in one line what the patch does.
- `Run:` is what `scripts/run-x86_64.sh` is given after the accelerator.
- `Expect:` is an extended regular expression, matched line by line against the run's log. There may be several, and any one match means the test caught the defect. Expectations are read only from ablation patches.

## Running one

```
scripts/ablate.sh ablations/642-widen-swap.patch ablations/642-direct-swapin.patch
scripts/ablate.sh --ab --boots 5 --accel tcg,kvm ablations/642-widen-swap.patch ablations/642-direct-swapin.patch
scripts/ablate.sh --check ablations/*.patch
scripts/ablate.sh --plan --ab --boots 5 --accel tcg,kvm ablations/642-widen-swap.patch ablations/642-direct-swapin.patch
```

`--plan` says what the same command without it would build and boot, and refuses what it would refuse, building nothing. The campaign on GitHub asks it before a run that takes fixes out starts on twenty runners (#656).

Each run takes place in a worktree under `$UROS_ABLATE_DIR` (default `~/uros-tests/ablations`). The build there is configured with the options of `$UROS_ABLATE_REF_BUILD` (default `$UROS_BUILD_DIR` if it is set, otherwise `uros/build-x86_64`), because this tree's configuration cannot be reproduced from the defaults. Each arm boots the build of its own worktree, whatever `UROS_BUILD_DIR` the caller exported. Every boot adds one line to `ablate.csv` in the same directory: tree, patches, arm, accelerator, the clock and power source the harness recorded, its exit status, and the verdict.

`--ab` alternates each boot of the ablated tree with one of the fixed tree, which has the widen patches but not the ablation. The ablated arm must be CAUGHT and the fixed arm must pass.

## Making one

Commit the fix first. Then:

```
scripts/ablate.sh --make <fix-commit> <path>... > ablations/<issue>-<what>.patch
```

This writes the reverse of the commit's changes to those paths, under a header to fill in. Trim it down to the code that brings the defect back, then check that it does: the ablated run must be CAUGHT, and the fixed run must pass.
