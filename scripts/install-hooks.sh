#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# install-hooks.sh -- point this clone's git hooks at scripts/hooks, once per clone.
#
# git does not install hooks from a repository by itself.  core.hooksPath is set in the clone's shared config, so
# every worktree of the clone takes it, and the hooks come and go with the tree: a git pull brings their changes.
# With core.hooksPath set, git no longer looks in .git/hooks: a hook kept there by hand is left aside, and this
# says so.  Today scripts/hooks holds pre-push, which asks the epic's queue (#949) before a push of the epic.
set -eu
TOP=$(git rev-parse --show-toplevel)
cd "$TOP"
COMMON=$(git rev-parse --git-common-dir)
for h in "$COMMON"/hooks/*; do
	[ -f "$h" ] || continue
	case $h in *.sample) continue ;; esac
	echo "install-hooks: $h is in .git/hooks and git will no longer run it"
done
git config core.hooksPath scripts/hooks
echo "install-hooks: core.hooksPath = $(git config core.hooksPath), for every worktree of $(cd "$COMMON/.." && pwd)"
