#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# disks-attached.sh LOG -- did every disk controller on the board attach? (#591)
#
# The harness that made the board says what it put there, in the run's
# conditions block: each controller, the disks on it, and in parentheses the
# block server's driver for it.
#
#   disks:        virtio-blk-pci with 1 disk (virtio_blk), ich9-ahci with 2 disks (ahci)
#
# Once the block server has started, each of them must be reported attached,
# with exactly that many disks:
#
#   blk: ahci at PCI 0:5.0 — 2 disk(s)
#
# 🔴 NOTHING ASKED THIS, AND EIGHT BOOTS THAT LOST THE AHCI CONTROLLER PASSED.
# The driver's own lines say `failed', `timed out' and `refused', which the
# verdicts' scans for WRONG and FAIL do not match; cap_test said the arm "did
# not run on that controller" and went on to ALL TESTS PASSED; and what reads
# the controller's disks either waited for them or was not started.  #577's
# ablations lost the controller twenty-five times, and eight of those boots
# were called passed.
#
# 🔑 THE REQUIREMENT COMES FROM THE BOARD, NOT FROM THE BOOT.  A line the boot
# prints cannot say what it should have found: a HAL scan that lost the device
# would take the requirement with it, the case #563 calls absent hardware the
# kernel cannot tell from hardware it failed to find.  The command line that
# put the controller there can.
#
# ⚠️ Asked only once the block server has started.  Entry 0 boots one program
# and the bench-only bundle carries no block server, on the same board.  The
# banner is printed before the server asks the HAL for anything, so a device
# the HAL lost cannot take the banner with it.
#
# ⚠️ The address is not compared.  The same controller is 0:5.0 on i440FX and
# 0:4.0 on q35, and q35 brings an AHCI controller of its own, at 0:31.2, with
# no disk on it, whose probe fails as it should.  The number of disks tells the
# two apart, and it has to be exact: a port that lost its disk is a controller
# that half attached.
#
# Exit status, and what it prints -- in the verdicts' words, for them to show:
#   0  every controller on the board attached: one line naming them, as the
#      log has them.  Or nothing to ask: the block server never started
#      (nothing printed), or the log records no board (said: before #591).
#   1  one did not: a FAILED paragraph naming it, with what the block server
#      and its driver said about it.
#   2  the log could not be read.
set -u

LOG=${1:-}
if [ ! -r "$LOG" ] || [ -d "$LOG" ]; then
	echo "disks-attached.sh: cannot read the log '$LOG'" >&2
	exit 2
fi

# i386's logs are captured with the serial port's carriage returns.
log() { tr -d '\r' < "$LOG"; }

log | grep -aq '^=== Block device server (HAL-driven) ===$' || exit 0

# Read inside the conditions block only: the guest's lines are not the board.
BOARD=$(log | sed -n '/^=== run conditions (#516) ===$/,/^=== end run conditions ===$/s/^  disks:        //p' | head -1)
if [ -z "$BOARD" ]; then
	echo "  disk controllers: not recorded -- this log predates #591, so none was required"
	exit 0
fi
# "virtio-blk-pci with 1 disk (virtio_blk)" -> "virtio_blk 1", one a line.
WANTED=$(printf '%s\n' "$BOARD" | tr ',' '\n' \
	| sed -n 's/.* with \([0-9][0-9]*\) disks\{0,1\} (\([a-z0-9_]*\)).*/\2 \1/p')
if [ -z "$WANTED" ]; then
	echo "  disk controllers: none on the board ($BOARD)"
	exit 0
fi

ATTACHED=""
MISSING=""
# The same driver twice on one board asks for two lines, hence the count; in
# the board's order.
while read -r drv n k; do
	have=$(log | grep -ac "^blk: $drv at PCI [0-9]*:[0-9]*\.[0-9]* — $n disk(s)$")
	if [ "$have" -ge "$k" ]; then
		ATTACHED="$ATTACHED${ATTACHED:+, }$(log | grep -a "^blk: $drv at PCI [0-9]*:[0-9]*\.[0-9]* — $n disk(s)$" \
			| sed 's/^blk: \(.*\) — \(.*\)$/\1 with \2/' | paste -sd, - | sed 's/,/, /g')"
	else
		MISSING="$MISSING$drv $n $k
"
	fi
done <<EOF
$(printf '%s\n' "$WANTED" | awk '!c[$0]++ {o[++m] = $0} END {for (i = 1; i <= m; i++) print o[i], c[o[i]]}')
EOF

if [ -z "$MISSING" ]; then
	echo "  disk controllers: $ATTACHED -- each one the board carries (#591)"
	exit 0
fi

echo "  FAILED: a disk controller this run put on the board never attached (#591)."
echo "          The board: $BOARD."
printf '%s' "$MISSING" | while read -r drv n k; do
	what=$([ "$n" = 1 ] && echo "1 disk" || echo "$n disks")
	echo "          $drv, with $what: no line 'blk: $drv at PCI … — $n disk(s)'$([ "$k" -gt 1 ] && echo " for each of $k")."
	echo "          What the block server and the driver said:"
	# Each controller the block server gave the driver, from the HAL's event
	# to the server's word on it, with the driver's last lines between, where
	# a probe that stopped says why; then the replay's count of what
	# attached.  The block server probes one controller at a time, so the
	# driver's lines between the two are that controller's.  The driver
	# prints under its own short name, virtio_blk's as `virtio'.
	said=$(log | awk -v drv="$drv" -v pfx="${drv%_blk}" '
		function flush(i) {
			if (kept > 6) print "…"
			for (i = (kept > 6 ? kept - 5 : 1); i <= kept; i++) print ring[i % 6]
		}
		/^blk: HAL event .* matched by / && $NF == drv { print; open = 1; kept = 0; next }
		open && index($0, "blk: " drv " ") == 1 && / at PCI / { flush(); print; open = 0; next }
		open && (index($0, drv ":") == 1 || index($0, drv " ") == 1 ||
			 index($0, pfx ":") == 1 || index($0, pfx " ") == 1) { ring[++kept % 6] = $0 }
		/^blk: HAL replay drained/ { if (open) flush(); open = 0; print }
		END { if (open) flush() }
	')
	if [ -n "$said" ]; then
		printf '%s\n' "$said" | sed 's/^/            /'
	else
		echo "            nothing"
	fi
	if ! log | grep -aq "matched by $drv\$"; then
		echo "          No HAL event was matched by $drv: the block server was never"
		echo "          told of the controller, which the HAL may have lost."
	fi
done
# A boot that stopped before the probes ends here too, and it must not read as
# a controller that refused: three answers, not two.
if ! log | grep -aq '^blk: HAL replay drained'; then
	echo "          The block server never said 'HAL replay drained', so it may"
	echo "          never have been asked to attach the controller at all."
fi
echo "          What reads those disks ran without them, or waited for them."
exit 1
