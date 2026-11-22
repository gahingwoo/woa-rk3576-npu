#!/bin/sh
# compare.sh - M4: the Windows run against the Linux reference
#
#   ./compare.sh m4-linux m4-windows
#
# The .text files are each arm's stdout without the timing lines (those
# start with '['). Same bytes, same text. win-run.ps1 does the same
# comparison on the board; this is for the desk.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only
set -u
L=${1:?usage: compare.sh LINUXDIR WINDOWSDIR}
W=${2:?usage: compare.sh LINUXDIR WINDOWSDIR}
bad=0
for a in short-cpu short-npu long-cpu long-npu logits-cpu logits-npu; do
	if [ ! -f "$L/$a.text" ] || [ ! -f "$W/$a.text" ]; then
		echo "$a: MISSING"; bad=1; continue
	fi
	if cmp -s "$L/$a.text" "$W/$a.text"; then
		echo "$a: SAME"
	else
		echo "$a: DIFFERENT"; bad=1
		diff "$L/$a.text" "$W/$a.text" | head -8
	fi
done
[ $bad = 0 ] && echo "M4 COMPARE: SAME TEXT" || echo "M4 COMPARE: DIFFERENT"
exit $bad
