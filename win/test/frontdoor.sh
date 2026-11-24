#!/bin/sh
# frontdoor.sh - the Windows front door and installer, run by pwsh on Linux
#
#   sh win/test/frontdoor.sh CHARSIU_BUILD_DIR MODEL.gguf
#
# charsiu.ps1 does not care what an .exe is, so charsiu's own Linux
# charsiu_run, charsiu_check and charsiu_serve stand in for the Windows ones
# under their Windows names. That runs every path of the script that is not
# Windows itself (PnP, the registry, the NPU): the ini, the zoo, pull with
# its sha256 check against a local stand-in for Hugging Face, the gate, list,
# rm, a question, the argument quoting, bench, serve on a port the system
# picks, and the installer's dry run in all three ways it can be started.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only
set -eu

B=$(cd "$1" && pwd)
MODEL=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
HERE=$(cd "$(dirname "$0")/.." && pwd)
CHARSIU_SRC=${CHARSIU_SRC:-$HERE/../external/charsiu}
PW=${PWSH:-pwsh}
T=$(mktemp -d)
SRV=""
cleanup() {
	[ -n "$SRV" ] && kill "$SRV" 2>/dev/null || true
	pkill -f "$T/bin/charsiu_serve.exe" 2>/dev/null || true
	rm -rf "$T"
}
trap cleanup EXIT
fail() { echo "FRONTDOOR FAIL: $*" >&2; exit 1; }

mkdir -p "$T/bin/bin" "$T/home" "$T/mirror" "$T/la" "$T/tmp"
for e in charsiu_run charsiu_check charsiu_serve; do cp "$B/$e" "$T/bin/$e.exe"; done
cp "$HERE/charsiu.ps1" "$HERE/charsiu-install.ps1" "$HERE/config.ini" "$T/bin/"
cp "$HERE/charsiu.cmd" "$T/bin/bin/"
sh "$HERE/zoo.sh" "$CHARSIU_SRC/scripts/charsiu-get" > "$T/bin/zoo.txt"
echo frontdoor-test > "$T/bin/VERSION"
[ "$(wc -l < "$T/bin/zoo.txt")" -gt 10 ] || fail "zoo.txt has $(wc -l < "$T/bin/zoo.txt") rows"

cs() { CHARSIU_HOME="$T/home" "$PW" -NoProfile -File "$T/bin/charsiu.ps1" "$@"; }

# ---- the zoo, as data and as a table
[ "$(cs --version)" = "charsiu frontdoor-test" ] || fail "--version"
cs pull --zoo > "$T/zoo.out"
grep -q "^dir|$T/home/models$" "$T/zoo.out" || fail "pull --zoo: no dir record"
grep -q '^zoo|smollm2-135m-q4|bartowski/SmolLM2-135M-Instruct-GGUF|SmolLM2-135M-Instruct-Q4_0.gguf|92|' "$T/zoo.out" \
	|| fail "pull --zoo: no smollm2 record"
[ "$(grep '^zoo|smolvlm-256m|' "$T/zoo.out" | awk -F'|' '{print NF}')" = 8 ] || fail "pull --zoo: a row is not 8 fields"
cs pull | grep -q 'qwen3-0.6b-q4' || fail "pull with no name lists the zoo"

# ---- pull through a stand-in for Hugging Face: the model the zoo calls
# smollm2-135m-q4 is served from a local directory, with its real sha256
REPO=bartowski/SmolLM2-135M-Instruct-GGUF
F=SmolLM2-135M-Instruct-Q4_0.gguf
mkdir -p "$T/mirror/$REPO/resolve/main" "$T/mirror/api/models/$REPO/tree"
ln -s "$MODEL" "$T/mirror/$REPO/resolve/main/$F"
SHA=$(sha256sum "$MODEL" | cut -d' ' -f1)
printf '[{"type":"file","oid":"x","size":1,"lfs":{"oid":"%s","size":1},"path":"%s"}]' "$SHA" "$F" \
	> "$T/mirror/api/models/$REPO/tree/main"
PORT=$((20000 + $$ % 20000))
(cd "$T/mirror" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
SRV=$!
sleep 1
export HF_ENDPOINT="http://127.0.0.1:$PORT" CHARSIU_NO_PROGRESS=1

cs pull smollm2-135m-q4 > "$T/pull.out" 2>&1 || { cat "$T/pull.out"; fail "pull"; }
grep -q 'sha256 matches' "$T/pull.out" || { cat "$T/pull.out"; fail "pull did not check the sha256"; }
[ -f "$T/home/models/$F" ] || fail "pull kept nothing"
grep -q "^path = $T/home/models/$F$" "$T/home/config.ini" || fail "pull did not record the model in the config"
cs pull smollm2-135m-q4 2>&1 | grep -q 'already here' || fail "a second pull fetched again"
cs list | grep -q "$F .* yes " || fail "list"

# ---- a question, the quoting, bench
CHARSIU_NO_NPU_HINT=1 cs 'Name one color.' -n 8 > "$T/q.out" 2> "$T/q.err" || { cat "$T/q.err"; fail "a question"; }
[ -s "$T/q.out" ] || fail "a question printed nothing"
cs --show 'say "hi" now' > "$T/show.out"
grep -q -- '-p "say \\"hi\\" now" --chat -q' "$T/show.out" || { cat "$T/show.out"; fail "--show quoting"; }
grep -q -- '--cache' "$T/show.out" || fail "the weight cache is not on by default"
grep -q 'CHARSIU_NPU_KMAX=1024' "$T/show.out" && grep -q 'CHARSIU_NPU_W4_GROUP=1024' "$T/show.out" \
	|| fail "--show: the NPU variables"
CHARSIU_NPU_KMAX=512 cs --show hi | grep -q 'from the environment, not the config: CHARSIU_NPU_KMAX' \
	|| fail "an exported variable did not win over the config"
CHARSIU_NO_NPU_HINT=1 cs bench 2>/dev/null | grep -q 'gen 64 tok in' || fail "bench"

# ---- serve on a port the system picks
CHARSIU_NO_NPU_HINT=1 CHARSIU_HOME="$T/home" "$PW" -NoProfile -File "$T/bin/charsiu.ps1" serve --port 0 \
	2> "$T/serve.err" &
for _ in $(seq 120); do grep -q 'ready on' "$T/serve.err" && break; sleep 1; done
SP=$(grep -o 'ready on http://127.0.0.1:[0-9]*' "$T/serve.err" | sed 's/.*://')
[ -n "$SP" ] && [ "$SP" != 0 ] || { cat "$T/serve.err"; fail "serve did not say its port"; }
grep -q 'weight cache:' "$T/serve.err" || fail "serve without the weight cache"
curl -fsS "http://127.0.0.1:$SP/health" | grep -q '"status":"ok"' || fail "health"
curl -fsSN "http://127.0.0.1:$SP/v1/chat/completions" \
	-d '{"messages":[{"role":"system","content":"Be brief."},{"role":"user","content":"Hello"}],"stream":true,"max_tokens":8}' \
	> "$T/sse.out"
grep -q '"delta":{"content":' "$T/sse.out" && grep -q '^data: \[DONE\]' "$T/sse.out" || fail "streaming"
timeout 1 curl -sN "http://127.0.0.1:$SP/v1/chat/completions" \
	-d '{"messages":[{"role":"user","content":"Count from 1 to 1000, one number a line."}],"stream":true,"max_tokens":2000}' >/dev/null || true
for _ in $(seq 20); do grep -q 'the client went away' "$T/serve.err" && break; sleep 1; done
grep -q 'the client went away' "$T/serve.err" || { tail "$T/serve.err"; fail "a client that left did not stop the answer"; }
pkill -f "$T/bin/charsiu_serve.exe" || true

# ---- rm keeps the model in use unless told twice, and takes its cache along
cs rm "$F" > /dev/null 2>&1 && fail "rm removed the model in use"
cs rm -f "$F" | grep -q 'removed' || fail "rm -f"
[ ! -f "$T/home/models/$F" ] && [ ! -f "$T/home/models/$F.wcache" ] || fail "rm left a file"

# ---- a sha256 that does not match deletes the download
sed -i "s/$SHA/$(printf '%064d' 0)/" "$T/mirror/api/models/$REPO/tree/main"
cs pull smollm2-135m-q4 > "$T/bad.out" 2>&1 && fail "a wrong sha256 was accepted"
grep -q 'Hugging Face publishes 0000' "$T/bad.out" || { cat "$T/bad.out"; fail "the mismatch message"; }
ls "$T/home/models" | grep -q . && fail "a download with a wrong sha256 was kept"

# ---- the installer's dry run, three ways in
export LOCALAPPDATA="$T/la" TEMP="$T/tmp" PROCESSOR_ARCHITECTURE=ARM64
"$PW" -NoProfile -File "$T/bin/charsiu-install.ps1" --dry-run > "$T/i1.out" 2>&1 || { cat "$T/i1.out"; fail "installer -File"; }
(cd "$T/bin" && CHARSIU_INSTALL_ARGS=--dry-run "$PW" -NoProfile -Command 'Get-Content ./charsiu-install.ps1 -Raw | iex') \
	> "$T/i2.out" 2>&1 || { cat "$T/i2.out"; fail "installer through iex"; }
(cd "$T/bin" && "$PW" -NoProfile -Command '& ([scriptblock]::Create((Get-Content ./charsiu-install.ps1 -Raw))) -DryRun --driver --user-access') \
	> "$T/i3.out" 2>&1 || { cat "$T/i3.out"; fail "installer through a scriptblock"; }
for f in i1 i2 i3; do
	grep -q 'DRY RUN. Nothing above was done' "$T/$f.out" || { cat "$T/$f.out"; fail "$f: no dry run summary"; }
	grep -q 'would download https://github.com/gahingwoo/woa-rk3576-npu/releases/latest/download/charsiu-windows-arm64.zip' "$T/$f.out" \
		|| fail "$f: no package download"
done
grep -q 'would pnputil /add-driver' "$T/i3.out" && grep -q 'stage only' "$T/i3.out" || fail "--driver dry run"
grep -q 'ConfigFlags |= 0x20' "$T/i3.out" || fail "--driver does not mark the device for reinstall"
grep -q 'UserAccess = 1' "$T/i3.out" || fail "--user-access dry run"
[ -z "$(ls "$T/la")" ] || fail "a dry run wrote into LOCALAPPDATA"

echo "FRONTDOOR: PASS"
