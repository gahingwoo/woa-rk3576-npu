#!/bin/sh
# zoo.sh CHARSIU-GET: charsiu's model list, as zoo.txt for the Windows front
# door. The list lives in charsiu's scripts/charsiu-get and nowhere else, so
# Windows and Linux offer the same models with the same sizes.
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu
sed -n "/^cat <<'ZOO'\$/,/^ZOO\$/p" "$1" | sed '1d;$d' | grep '|'
