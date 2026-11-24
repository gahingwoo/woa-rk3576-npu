#!/bin/sh
# linux-ref.sh - M4, the Linux reference: charsiu through rocket on the board
#
#   ./linux-ref.sh MODEL.gguf [OUTDIR]
#
# Run as root on the board under Armbian with rocket (docs/dev/testing.md; first written for docs/history/2026-11-22-m4.md, part A),
# from the unpacked charsiu-m4-arm64 artifact: it uses linux/charsiu_run and
# linux/charsiu_matmul beside it (the same charsiu commit as the Windows
# exes, static) and long.txt. Writes OUTDIR (default ./m4-linux) and
# OUTDIR.tar.gz. Every arm is greedy and runs with a clean CHARSIU_*
# environment; the NPU arms set exactly what charsiu-runner sets.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${1:?usage: linux-ref.sh MODEL.gguf [OUTDIR]}
OUT=${2:-./m4-linux}
RUN=$HERE/linux/charsiu_run
MATMUL=$HERE/linux/charsiu_matmul
LONG=$HERE/long.txt
SHA=3f87a880027e7b9ea8e0da9e4009584336f352af444a0e6e5c20721ac4c7ffd1
PROMPT="Write a short story about a lighthouse keeper who finds a message in a bottle."

die() { echo "linux-ref: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (the NPU node is root's)"
[ -x "$RUN" ] && [ -x "$MATMUL" ] && [ -f "$LONG" ] || die "run from the unpacked artifact (linux/, long.txt)"
ls /dev/accel/accel* >/dev/null 2>&1 || die "no /dev/accel node: is rocket loaded and the NPU enabled in the DTB?"
mkdir -p "$OUT"

# no experiment switch may leak in from the shell
for v in $(env | sed -n 's/^\(CHARSIU_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done

echo "linux-ref: checking the model (1.3 GB)"
got=$(sha256sum "$MODEL" | cut -d' ' -f1)
[ "$got" = "$SHA" ] || die "$MODEL is not Llama-3.2-1B-Instruct-Q8_0.gguf (sha256 $got)"

{
	uname -a
	cat /proc/cmdline
	date -u
	"$RUN" --version 2>&1 | head -1
	sha256sum "$RUN" "$MATMUL"
	free -m
	ls -l /dev/accel/
	lsmod | grep -i rocket
} > "$OUT/system.txt" 2>&1 || true

NPU="CHARSIU_NPU=1 CHARSIU_NPU_QUANT=1 CHARSIU_NPU_W4V=1 CHARSIU_NPU_KMAX=1024 CHARSIU_NPU_W4_GROUP=1024 CHARSIU_NPU_MAXN=262144 CHARSIU_COEF_ELEMS=65536"

arm() {     # name, env, args...
	name=$1; e=$2; shift 2
	echo "linux-ref: $name"
	# shellcheck disable=SC2086
	env $e "$RUN" "$MODEL" "$@" -t 4 > "$OUT/$name.txt" 2> "$OUT/$name.err" || echo "exit $?" >> "$OUT/$name.err"
	grep -v '^\[' "$OUT/$name.txt" > "$OUT/$name.text" || true
}

echo "linux-ref: charsiu_matmul 1 256 64"
"$MATMUL" 1 256 64 > "$OUT/matmul.txt" 2>&1 || echo "exit $?" >> "$OUT/matmul.txt"

arm short-cpu  ""     -p "$PROMPT" -n 128 -c 512
arm short-npu  "$NPU" -p "$PROMPT" -n 128 -c 512
arm short-npu2 "$NPU" -p "$PROMPT" -n 128 -c 512
arm long-cpu   ""     -f "$LONG" -n 64 -c 1024
arm long-npu   "$NPU" -f "$LONG" -n 64 -c 1024
arm logits-cpu ""     -p "$PROMPT" -c 512 --logits 10
arm logits-npu "$NPU" -p "$PROMPT" -c 512 --logits 10

( cd "$OUT" && md5sum ./*.text > text.md5 )
cat "$OUT/text.md5"
if cmp -s "$OUT/short-npu.text" "$OUT/short-npu2.text"; then
	echo "linux-ref: the NPU arm repeats itself byte for byte"
else
	echo "linux-ref: THE NPU ARM DOES NOT REPEAT ITSELF; the Windows comparison cannot be exact"
fi
tar -C "$(dirname "$OUT")" -czf "$OUT.tar.gz" "$(basename "$OUT")"
echo "linux-ref: done, keep $OUT.tar.gz for the Windows run"
