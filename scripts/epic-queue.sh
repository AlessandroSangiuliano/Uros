#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# epic-queue.sh -- the epic's queue (#949): who is merge-testing a union over epic/v0.3.0-x86-64, and who waits.
#
# A union is merge-tested over the epic as it was when the union was made.  If another union is merged and
# pushed meanwhile, the base moves under the test, and the union has to be made again and tested again: on
# 14/11 #940's merge test at -s 4 stopped because #937 and #938 had been merged during its -s 1, and on 28/10
# the same happened with another session.  So whoever merge-tests a union takes the queue, and whoever finds it
# taken neither starts a merge test nor pushes the epic: they wait, or ask the holder to fold their branches into
# its next union, one union and one merge test.
#
# The state lives in the body of issue #949 on GitHub, between the lines <!-- coda-epic --> and
# <!-- /coda-epic -->, so that every session reads and writes the same one and anyone can see it there.
#
# Usage:
#   epic-queue.sh show [WHO]                       the state; exit 0 free, 1 held by another, 2 held by WHO,
#                                                  3 unreadable
#   epic-queue.sh take WHO UNION BASE UNTIL [NOTE] take it if it is free or WHO's already; exit 1 if another's
#   epic-queue.sh free WHO [NOTE]                  free it if it is WHO's; exit 1 if another's
#   epic-queue.sh wait WHO BRANCHES [NOTE]         a WAITING line for WHO, in place of the one there was
#   epic-queue.sh leave WHO                        take WHO's WAITING line out
# WHO is host:union-branch, for example simo-victus:prova-unione-940-3: no two sessions test the same union.
#
# Needs gh, logged in.  scripts/hooks/pre-push asks it before a push of the epic (scripts/install-hooks.sh).
set -u
QUEUE=949
REPO=AlessandroSangiuliano/Uros
OPEN='<!-- coda-epic -->'
CLOSE='<!-- /coda-epic -->'

# The body in BODY, the block in BLOCK.  Carriage returns go: a body edited on the web may carry them.
read_queue() {
	BODY=$(gh issue view "$QUEUE" --repo "$REPO" --json body --jq .body 2>/dev/null | tr -d '\r') || return 1
	printf '%s\n' "$BODY" | grep -qxF "$OPEN" && printf '%s\n' "$BODY" | grep -qxF "$CLOSE" || return 1
	BLOCK=$(printf '%s\n' "$BODY" | awk -v a="$OPEN" -v b="$CLOSE" '$0 == a { f = 1; next } $0 == b { f = 0 } f')
}
field() { printf '%s\n' "$BLOCK" | sed -n "s/^$1: //p" | head -n 1; }
waiting_but() { printf '%s\n' "$BLOCK" | grep '^WAITING: ' | grep -vF "WAITING: $1 " || true; }
# The new block in the body, in place of the old one.
write_queue() {
	printf '%s\n' "$BODY" | NEW="$1" awk -v a="$OPEN" -v b="$CLOSE" '
		$0 == a { print; print ENVIRON["NEW"]; skip = 1; next }
		$0 == b { skip = 0 }
		!skip { print }' | gh issue edit "$QUEUE" --repo "$REPO" --body-file - > /dev/null
}
now() { date '+%F %H:%M'; }
say() {
	if [ "$(field STATE)" = held ]; then
		echo "epic queue (#$QUEUE): HELD by $(field HOLDER): union $(field UNION) over $(field BASE), since" \
		     "$(field SINCE), expected end $(field UNTIL) -- $(field NOTE)"
	else
		echo "epic queue (#$QUEUE): free"
	fi
	printf '%s\n' "$BLOCK" | grep '^WAITING: ' | sed 's/^/  /' || true
}

cmd=${1:-show}; shift || true
read_queue || { echo "epic-queue: issue #$QUEUE cannot be read, or has no $OPEN block"; exit 3; }
state=$(field STATE); holder=$(field HOLDER)

case $cmd in
show)
	WHO=${1:-}
	say
	if [ "$state" != held ]; then exit 0; fi
	if [ -n "$WHO" ] && [ "$holder" = "$WHO" ]; then exit 2; fi
	exit 1 ;;
take)
	WHO=${1:?who}; UNION=${2:?union}; BASE=${3:?base}; UNTIL=${4:?expected end}; NOTE=${5:-}
	if [ "$state" = held ] && [ "$holder" != "$WHO" ]; then say; exit 1; fi
	since=$(field SINCE); [ "$state" = held ] && [ -n "$since" ] || since=$(now)
	new=$(printf 'STATE: held\nHOLDER: %s\nUNION: %s\nBASE: %s\nSINCE: %s\nUNTIL: %s\nNOTE: %s' \
		"$WHO" "$UNION" "$BASE" "$since" "$UNTIL" "$NOTE")
	w=$(waiting_but "$WHO"); [ -n "$w" ] && new="$new"$'\n'"$w"
	write_queue "$new" || { echo "epic-queue: writing failed"; exit 3; }
	# Read back: two sessions taking it at once leave one holder, and the other is told.
	read_queue && [ "$(field HOLDER)" = "$WHO" ] || { echo "epic-queue: not $WHO's after the write"; say; exit 1; }
	say ;;
free)
	WHO=${1:?who}; NOTE=${2:-}
	if [ "$state" != held ]; then say; exit 0; fi
	if [ "$holder" != "$WHO" ]; then echo "epic-queue: not $WHO's"; say; exit 1; fi
	new='STATE: free'
	[ -n "$NOTE" ] && new="$new"$'\n'"LAST: $WHO, $(now): $NOTE"
	w=$(waiting_but "$WHO"); [ -n "$w" ] && new="$new"$'\n'"$w"
	write_queue "$new" || { echo "epic-queue: writing failed"; exit 3; }
	read_queue && say ;;
wait|leave)
	WHO=${1:?who}
	new=$(printf '%s\n' "$BLOCK" | grep -v '^WAITING: ' || true)
	w=$(waiting_but "$WHO"); [ -n "$w" ] && new="$new"$'\n'"$w"
	if [ "$cmd" = wait ]; then
		BRANCHES=${2:?branches}; NOTE=${3:-}
		new="$new"$'\n'"WAITING: $WHO $BRANCHES (since $(now))${NOTE:+ -- $NOTE}"
	fi
	write_queue "$new" || { echo "epic-queue: writing failed"; exit 3; }
	read_queue && say ;;
*)
	echo "usage: epic-queue.sh show [WHO] | take WHO UNION BASE UNTIL [NOTE] | free WHO [NOTE] |" \
	     "wait WHO BRANCHES [NOTE] | leave WHO"
	exit 2 ;;
esac
