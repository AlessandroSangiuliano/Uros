#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ci/pull-results.sh -- the campaign's rows and failed logs, brought into
# ~/uros-tests (#653).
#
# The campaign (.github/workflows/campaign.yml) keeps each job's rows and the
# logs of its WRONG boots as an artifact on GitHub, for thirty days.  This
# takes every completed run it has not taken before, appends its rows to
# ~/uros-tests/ci-campaign.csv, where they accumulate with the home machines'
# rows, and keeps each job's artifact under ~/uros-tests/ci/<run-id>/job<N>/.
# A row's log column then names the local copy of a WRONG boot's log, and is
# "-" for a boot that was as it should be, whose log stayed on the runner.
# Times are the runner's, in UTC.  Compare rows within one job only: the
# runner's clock is not ours to set.
#
# A run that took fixes out (`ablate`, #656) has other rows, one per boot of
# either arm, and they go to ~/uros-tests/ci-ablate.csv; every one of those
# boots' logs comes with them, so their log column always names a local file.
#
# Nothing is imported twice.  The runs taken are listed in ci/imported-runs,
# and a run whose rows are already in the CSV -- an import stopped between its
# append and its record -- is recorded without being appended again.
#
# Usage:  scripts/ci/pull-results.sh [--limit N] [--dir DIR]
#   --limit N  how many of the latest completed runs to look at (default 30)
#   --dir DIR  where ci-campaign.csv and ci/ live (default ~/uros-tests)
#
# Exit status: 0 every new run imported, or none was new · 1 a run could not be
# listed, downloaded or read, and is left for the next call · 2 the command
# line is wrong, gh is missing, or ci-campaign.csv or ci-ablate.csv has a
# header other than the one job.sh writes.
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HEADER=date,time,run_id,job,round,tree,cpu_model,mhz,accel,entry,smp,opt,verdict,log
AB_HEADER=date,time,run_id,job,tree,cpu_model,mhz,patches,arm,boot,accel,run,harness_status,verdict,log
LIMIT=30
DIR=$HOME/uros-tests

die() {
	echo "pull-results: $2" >&2
	exit "$1"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--limit|--dir)
		[ $# -ge 2 ] || die 2 "$1 needs a value"
		case "$1" in
		--limit)	LIMIT=$2 ;;
		--dir)		DIR=$2 ;;
		esac
		shift 2 ;;
	-h|--help)	sed -n '/^# Usage:/,/^# header other than/p' "$0"; exit 0 ;;
	*)		die 2 "unknown argument '$1' (--help says what it takes)" ;;
	esac
done
case "$LIMIT" in
''|*[!0-9]*|0)	die 2 "--limit takes a number of runs, not '$LIMIT'" ;;
esac
command -v gh >/dev/null 2>&1 || die 2 "gh is not installed, and the runs are reached through it"

CSV=$DIR/ci-campaign.csv
CI=$DIR/ci
DONE=$CI/imported-runs
mkdir -p "$CI" || die 2 "cannot create $CI"
touch "$DONE"
AB_CSV=$DIR/ci-ablate.csv
for f in "$CSV:$HEADER" "$AB_CSV:$AB_HEADER"; do
	file=${f%%:*}
	head=${f#*:}
	if [ -s "$file" ]; then
		[ "$(head -n 1 "$file")" = "$head" ] ||
			die 2 "$file has another header than the one job.sh writes: rows appended to it would not line up"
	else
		echo "$head" > "$file"
	fi
done
# Downloads land beside the results, not in /tmp, and go once read.
STAGE=$(mktemp -d "$CI/.incoming.XXXXXX") || die 2 "cannot create a directory in $CI"
trap 'rm -rf "$STAGE"' EXIT

# Boots and WRONG by entry and accelerator, from rows on stdin.  An entry is its
# number with the options and processors that make it a line of its own in the
# merge test, so 14 at -smp 1 is not added into 14 at -smp 4.
summary() {
	awk -F, '
	{
		k = sprintf("entry %s%s -smp %s %s", $10, ($12 == "" ? "" : " (" $12 ")"), $11, $9)
		n[k]++
		all++
		if ($13 ~ /^WRONG/) {
			w[k]++
			wrong++
			list = list sprintf("  WRONG  run %s job %s round %s, %s: %s\n", $3, $4, $5, k, $14)
		}
	}
	END {
		cmd = "sort -k2,2n -k3"
		for (k in n)
			printf "  %-38s %6d boots %5d WRONG\n", k, n[k], w[k] + 0 | cmd
		close(cmd)
		printf "%s", list
		printf "  in all: %d boots, %d WRONG\n", all, wrong
	}'
}

# An A/B's boots by patches, processor, arm, accelerator and verdict, from rows
# on stdin: a defect only some runners show is read off the processor column.
ab_summary() {
	awk -F, '{ n[$8 " | " $6 " | " $9 " " $11 " " $14]++ }
	END { for (k in n) printf "  %4d  %s\n", n[k], k | "sort -t\"|\" -k1,2"; }'
}

# One downloaded run: its rows checked, every job's artifact kept under
# ci/<run-id>/job<N>/, the rows appended, the run recorded.  Everything is
# checked before anything is kept, because a header or a row of another shape
# would put values under the wrong columns for good.
import() {	# run-id description artifacts-the-api-listed
	local id=$1 what=$2 listed=$3 d j rows=$STAGE/$1.rows abrows=$STAGE/$1.abrows jobs=()
	: > "$rows"
	: > "$abrows"
	for d in "$STAGE/$id"/campaign-job-*/; do
		[ -d "$d" ] || continue
		jobs+=("$d")
		j=${d%/}
		j=${j##*/campaign-job-}
		if [ -f "$d/ablate.csv" ]; then
			if [ "$(head -n 1 "$d/ablate.csv")" != "$AB_HEADER" ]; then
				echo "pull-results: $what: job $j's A/B rows have another header than this script reads; not imported" >&2
				return 1
			fi
			if ! tail -n +2 "$d/ablate.csv" | awk -F, -v OFS=, -v dest="$CI/$id/job$j" '
				NF != 15 { exit 1 }
				{ $15 = dest "/" $15; print }' >> "$abrows"; then
				echo "pull-results: $what: job $j has an A/B row without the 15 columns of the header; not imported" >&2
				return 1
			fi
		fi
		[ -f "$d/boots.csv" ] || continue
		if [ "$(head -n 1 "$d/boots.csv")" != "$HEADER" ]; then
			echo "pull-results: $what: job $j's rows have another header than this script reads; not imported" >&2
			return 1
		fi
		if ! tail -n +2 "$d/boots.csv" | awk -F, -v OFS=, -v dest="$CI/$id/job$j" '
			NF != 14 { exit 1 }
			{ $14 = ($14 ~ /^failed\//) ? dest "/" $14 : "-"; print }' >> "$rows"; then
			echo "pull-results: $what: job $j has a row without the 14 columns of the header; not imported" >&2
			return 1
		fi
	done
	# A download that holds other than what the API listed is not a run read:
	# gh may have put the artifacts somewhere else, or some went missing.
	if [ ${#jobs[@]} -ne "$listed" ]; then
		echo "pull-results: $what: GitHub listed $listed artifact(s) and the download holds ${#jobs[@]}; not imported" >&2
		return 1
	fi
	for d in "${jobs[@]}"; do
		j=${d%/}
		j=${j##*/campaign-job-}
		if ! { mkdir -p "$CI/$id/job$j" && cp -a "$d." "$CI/$id/job$j/"; }; then
			echo "pull-results: $what: cannot copy job $j's artifact into $CI/$id/job$j; not imported" >&2
			return 1
		fi
		[ -f "$d/boots.csv" ] || [ -f "$d/ablate.csv" ] ||
			echo "pull-results: $what: job $j left no rows; what it left is in $CI/$id/job$j"
	done
	if grep -q "^[^,]*,[^,]*,$id," "$CSV"; then
		echo "pull-results: $what: its rows are already in $CSV, from an import stopped before its record; recorded, not appended again"
	else
		cat "$rows" >> "$CSV"
	fi
	if [ -s "$abrows" ]; then
		if grep -q "^[^,]*,[^,]*,$id," "$AB_CSV"; then
			echo "pull-results: $what: its A/B rows are already in $AB_CSV, from an import stopped before its record; recorded, not appended again"
		else
			cat "$abrows" >> "$AB_CSV"
		fi
		echo "pull-results: $what: an A/B, $(wc -l < "$abrows") boots:"
		ab_summary < "$abrows"
	fi
	echo "$id $(date +%F) $(wc -l < "$rows") rows, $(wc -l < "$abrows") A/B rows" >> "$DONE"
	# The merge test's line, unless the run was an A/B and had none of its rows.
	[ -s "$rows" ] || [ ! -s "$abrows" ] || return 0
	awk -F, -v what="$what" -v jobs=${#jobs[@]} '
		{ n++; if ($13 ~ /^WRONG/) w++; acc[$9] = 1; tree[$6] = 1 }
		END {
			a = ""; for (k in acc) a = a " " k
			t = ""; for (k in tree) t = t " " k
			printf "pull-results: %s: %d job(s), %d boots, %d WRONG; tree%s; accelerators%s\n",
				what, jobs, n, w, (t == "" ? " -" : t), (a == "" ? " -" : a)
		}' "$rows"
}

# gh finds the repository from the checkout's remote.
cd "$REPO" || die 2 "cannot enter $REPO"
RUNS=$(gh run list --workflow campaign.yml --status completed --limit "$LIMIT" \
	--json databaseId,event,conclusion,headSha \
	--jq '.[] | "\(.databaseId) \(.event) \(.conclusion) \(.headSha[0:8])"') ||
	die 1 "gh could not list the campaign's runs"

# Oldest first, so that the file accumulates in the order the runs were made.
STATUS=0
NEW=""
LISTED=0
OLDEST_NEW=0
while read -r id event conclusion sha; do
	[ -n "$id" ] || continue
	LISTED=$((LISTED + 1))
	if awk -v id="$id" '$1 == id { f = 1 } END { exit !f }' "$DONE"; then
		continue
	fi
	[ $LISTED = 1 ] && OLDEST_NEW=1
	what="run $id ($event, $conclusion, $sha)"
	n=$(gh api "repos/{owner}/{repo}/actions/runs/$id/artifacts?per_page=100" \
		--jq '[.artifacts[] | select((.name | startswith("campaign-job-")) and (.expired | not))] | length' \
		< /dev/null) || {
		echo "pull-results: $what: gh could not list its artifacts; left for the next call" >&2
		STATUS=1
		continue
	}
	# A count that is not a number would make the comparison in import() an
	# error, which `if' reads as agreement.
	case "$n" in
	''|*[!0-9]*)
		echo "pull-results: $what: gh answered '$n' for its number of artifacts; left for the next call" >&2
		STATUS=1
		continue ;;
	esac
	if [ "$n" = 0 ]; then
		# Nothing to wait for: an expired artifact does not come back, and a
		# run stopped before any job reached its upload never had one.
		echo "pull-results: $what: no artifact to download, expired or never kept; recorded"
		echo "$id $(date +%F) no artifact" >> "$DONE"
		continue
	fi
	if ! gh run download "$id" --pattern 'campaign-job-*' --dir "$STAGE/$id" < /dev/null; then
		echo "pull-results: $what: the download failed; left for the next call" >&2
		STATUS=1
		continue
	fi
	if import "$id" "$what" "$n"; then
		NEW="$NEW $id"
	else
		STATUS=1
	fi
done < <(tac <<< "$RUNS")

# A full page of runs whose oldest was new as well says nothing about the runs
# before it, which may never have been looked at.
if [ "$LISTED" -ge "$LIMIT" ] && [ $OLDEST_NEW = 1 ]; then
	echo "pull-results: the oldest of the $LISTED runs looked at was new too, so older ones may be waiting: --limit more" >&2
fi
if [ -z "$NEW" ]; then
	echo "pull-results: nothing new"
else
	NEW_ROWS=$(awk -F, -v ids=" $NEW " 'NR > 1 && index(ids, " " $3 " ")' "$CSV")
	if [ -n "$NEW_ROWS" ]; then
		echo "pull-results: the runs imported now, by entry and accelerator:"
		summary <<< "$NEW_ROWS"
	fi
fi
echo "pull-results: $CSV holds $(($(wc -l < "$CSV") - 1)) boots, $AB_CSV $(($(wc -l < "$AB_CSV") - 1))"
exit $STATUS
