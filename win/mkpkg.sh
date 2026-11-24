#!/bin/sh
# mkpkg.sh EXE_DIR OUT_DIR: charsiu-windows-arm64.zip and its .sha256
#
# The package charsiu-install.ps1 installs: the front door, the three
# programs, the zoo, the config template and bin\charsiu.cmd (the one folder
# that goes on PATH). Run from the repository root, in CI.
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu
EXE=$1
OUT=$2
TOP=$(pwd)
CH=$TOP/external/charsiu
NAME=charsiu-windows-arm64
S=$OUT/$NAME
rm -rf "$S"
mkdir -p "$S/bin"
for e in charsiu_run charsiu_serve charsiu_check; do cp "$EXE/$e.exe" "$S/"; done
cp win/charsiu.ps1 win/charsiu-install.ps1 win/config.ini "$S/"
cp win/charsiu.cmd "$S/bin/"
cp tools/rknpu-perfcounters.ps1 "$S/"
sh win/zoo.sh "$CH/scripts/charsiu-get" > "$S/zoo.txt"
printf '%s-win-%s\n' "$(git -C "$CH" describe --always --abbrev=12)" "$(git describe --always --dirty --abbrev=12)" > "$S/VERSION"
cp LICENSE "$S/LICENSE.txt"
cp port/libm/LICENSE.optimized-routines "$S/"
cat > "$S/README.txt" <<TXT
charsiu for Windows on Arm, $(cat "$S/VERSION")

Install:  powershell -NoProfile -ExecutionPolicy Bypass -File charsiu-install.ps1 --zip <this zip>
or, from the release:  irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1 | iex

Then, in a new terminal:  charsiu   (charsiu --help lists the commands)
The NPU driver: docs/use.md in https://github.com/gahingwoo/woa-rk3576-npu
TXT
# CRLF for the files Notepad and cmd read
for f in README.txt bin/charsiu.cmd; do sed -i 's/\r*$/\r/' "$S/$f"; done
(cd "$S" && find . -type f | sort > ../manifest.txt && sha256sum ./*.exe > SHA256SUMS)
rm -f "$OUT/$NAME.zip"
(cd "$S" && zip -qr -X "../$NAME.zip" .)
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" && cat "$NAME.zip.sha256")
