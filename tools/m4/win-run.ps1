# win-run.ps1 - M4, the Windows run: charsiu through rknpu.sys on the board
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File win-run.ps1 -Model D:\Llama-3.2-1B-Instruct-Q8_0.gguf [-Linux .\m4-linux] [-Out .\m4-windows] [-SkipM3] [-NpuOnly]
#
# As administrator, from the unpacked charsiu-m4-arm64 artifact, with
# rknputest.exe from rknpu-m4-arm64 beside it or on PATH (docs/history/2026-11-22-m4.md, part
# B). The same arms as linux-ref.sh, with the same arguments, and the same
# .text files (stdout without the lines that start with '['). With -Linux
# (the unpacked m4-linux directory) it compares the two at the end.
#
# Every program runs through cmd.exe so its stdout reaches the file as bytes:
# Windows PowerShell's own '>' would re-encode it as UTF-16.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Model,
    [string]$Linux = '',
    [string]$Out = '.\m4-windows',
    [switch]$SkipM3,
    [switch]$NpuOnly
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $here 'charsiu_run.exe'
$matmul = Join-Path $here 'charsiu_matmul.exe'
$long = Join-Path $here 'long.txt'
$sha = '3f87a880027e7b9ea8e0da9e4009584336f352af444a0e6e5c20721ac4c7ffd1'
$prompt = 'Write a short story about a lighthouse keeper who finds a message in a bottle.'
$crumbs = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0\Device Parameters'

function Die($m) { Write-Host "win-run: $m" -ForegroundColor Red; exit 1 }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die 'run as administrator (\\.\RKNPU is administrators-only)'
}
foreach ($f in $run, $matmul, $long) { if (-not (Test-Path $f)) { Die "missing ${f}: run from the unpacked artifact" } }
$rkt = Join-Path $here 'rknputest.exe'
if (-not (Test-Path $rkt)) { $rkt = (Get-Command rknputest.exe -ErrorAction SilentlyContinue).Source }
if (-not $rkt) { Die 'rknputest.exe not found (copy it from rknpu-m4-arm64 beside this script)' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# no experiment switch may leak in from the shell
Get-ChildItem Env: | Where-Object { $_.Name -like 'CHARSIU_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }

Write-Host 'win-run: checking the model (1.3 GB)'
$got = (Get-FileHash -Algorithm SHA256 $Model).Hash.ToLower()
if ($got -ne $sha) { Die "$Model is not Llama-3.2-1B-Instruct-Q8_0.gguf (sha256 $got)" }

# One command line through cmd.exe, from a batch file, so neither Windows
# PowerShell's quoting of native arguments nor its '>' touches anything.
$bat = Join-Path $Out 'run.cmd'
function Cmd($line, $stdout, $stderr) {
    $body = "@echo off`r`n$line > `"$stdout`" 2> `"$stderr`"`r`nexit /b %errorlevel%`r`n"
    [IO.File]::WriteAllText($bat, $body, [Text.Encoding]::ASCII)
    & cmd.exe /d /c "`"$bat`"" | Out-Host
    return $LASTEXITCODE
}

# The arm's stdout without the lines that start with '[', byte for byte,
# as grep -v '^\[' writes it on Linux.
function Write-Text($src, $dst) {
    $b = [IO.File]::ReadAllBytes($src)
    $o = New-Object IO.MemoryStream
    $start = 0
    for ($i = 0; $i -le $b.Length; $i++) {
        if ($i -eq $b.Length -or $b[$i] -eq 10) {
            $len = $i - $start
            if ($len -gt 0 -or $i -lt $b.Length) {
                if (-not ($len -gt 0 -and $b[$start] -eq 91)) {
                    $o.Write($b, $start, $len)
                    $o.WriteByte(10)
                }
            }
            $start = $i + 1
        }
    }
    [IO.File]::WriteAllBytes($dst, $o.ToArray())
}

# system
$sys = Join-Path $Out 'system.txt'
& {
    Get-CimInstance Win32_OperatingSystem | Format-List Caption, Version, BuildNumber, TotalVisibleMemorySize, FreePhysicalMemory
    Get-CimInstance Win32_ComputerSystem | Format-List Manufacturer, Model, TotalPhysicalMemory
    Get-Date -Format o
    Get-FileHash -Algorithm SHA256 $run, $matmul, $rkt | Format-Table -AutoSize
    pnputil /enum-devices /instanceid 'ACPI\RKCP3576\0' /drivers
} *>&1 | Out-File -Encoding ascii $sys
Cmd "`"$run`" --version" (Join-Path $Out 'version.txt') (Join-Path $Out 'version.err') | Out-Null

Write-Host 'win-run: rknputest info'
Cmd "`"$rkt`" info" (Join-Path $Out 'info-before.txt') (Join-Path $Out 'info-before.err') | Out-Null
if (-not (Select-String -Quiet -Path (Join-Path $Out 'info-before.txt') -Pattern '^m4: version')) {
    Die "the driver is not the M4 build (see $Out\info-before.txt); install rknpu-m4-arm64 first"
}
if (-not $SkipM3) {
    Write-Host 'win-run: rknputest m3 (the M3 ladder, on the M4 driver)'
    $rc = Cmd "`"$rkt`" m3" (Join-Path $Out 'm3.txt') (Join-Path $Out 'm3.err')
    if ($rc -ne 0) { Die "rknputest m3 failed ($rc): stop here and send $Out" }
}

Write-Host 'win-run: charsiu_matmul 1 256 64'
Cmd "`"$matmul`" 1 256 64" (Join-Path $Out 'matmul.txt') (Join-Path $Out 'matmul.err') | Out-Null

$npu = @{ CHARSIU_NPU = '1'; CHARSIU_NPU_QUANT = '1'; CHARSIU_NPU_W4V = '1'; CHARSIU_NPU_KMAX = '1024';
          CHARSIU_NPU_W4_GROUP = '1024'; CHARSIU_NPU_MAXN = '262144'; CHARSIU_COEF_ELEMS = '65536';
          CHARSIU_RKNPU_STATS = '1' }

function Arm($name, $useNpu, $argline) {
    Write-Host "win-run: ${name}"
    if ($useNpu) { foreach ($k in $npu.Keys) { Set-Item "Env:$k" $npu[$k] } }
    $t0 = Get-Date
    $rc = Cmd "`"$run`" `"$Model`" $argline -t 4" (Join-Path $Out "$name.txt") (Join-Path $Out "$name.err")
    $s = [int]((Get-Date) - $t0).TotalSeconds
    if ($useNpu) { foreach ($k in $npu.Keys) { Remove-Item "Env:$k" } }
    if ($rc -ne 0) { Add-Content (Join-Path $Out "$name.err") "exit $rc" }
    Write-Text (Join-Path $Out "$name.txt") (Join-Path $Out "$name.text")
    Write-Host "  exit $rc, $s s"
    reg query $crumbs | Out-File -Encoding ascii (Join-Path $Out "$name.crumbs.txt")
}

$p = "-p `"$prompt`""
if (-not $NpuOnly) { Arm 'short-cpu' $false "$p -n 128 -c 512" }
Arm 'short-npu' $true "$p -n 128 -c 512"
Arm 'short-npu2' $true "$p -n 128 -c 512"
if (-not $NpuOnly) { Arm 'long-cpu' $false "-f `"$long`" -n 64 -c 1024" }
Arm 'long-npu' $true "-f `"$long`" -n 64 -c 1024"
if (-not $NpuOnly) { Arm 'logits-cpu' $false "$p -c 512 --logits 10" }
Arm 'logits-npu' $true "$p -c 512 --logits 10"

Cmd "`"$rkt`" info" (Join-Path $Out 'info-after.txt') (Join-Path $Out 'info-after.err') | Out-Null
reg query $crumbs | Out-File -Encoding ascii (Join-Path $Out 'crumbs.txt')

# the .text files' MD5s, in md5sum's format
$lines = Get-ChildItem (Join-Path $Out '*.text') | Sort-Object Name | ForEach-Object {
    '{0}  ./{1}' -f (Get-FileHash -Algorithm MD5 $_.FullName).Hash.ToLower(), $_.Name
}
[IO.File]::WriteAllText((Join-Path $Out 'text.md5'), (($lines -join "`n") + "`n"))
$lines | ForEach-Object { Write-Host $_ }
if ((Get-FileHash -Algorithm MD5 (Join-Path $Out 'short-npu.text')).Hash -eq (Get-FileHash -Algorithm MD5 (Join-Path $Out 'short-npu2.text')).Hash) {
    Write-Host 'win-run: the NPU arm repeats itself byte for byte'
} else {
    Write-Host 'win-run: THE NPU ARM DOES NOT REPEAT ITSELF' -ForegroundColor Yellow
}

if ($Linux) {
    $bad = 0
    foreach ($a in 'short-cpu', 'short-npu', 'long-cpu', 'long-npu', 'logits-cpu', 'logits-npu') {
        $l = Join-Path $Linux "$a.text"
        $w = Join-Path $Out "$a.text"
        if (-not (Test-Path $l) -or -not (Test-Path $w)) { Write-Host "${a}: MISSING"; $bad = 1; continue }
        if ((Get-FileHash -Algorithm MD5 $l).Hash -eq (Get-FileHash -Algorithm MD5 $w).Hash) {
            Write-Host "${a}: SAME"
        } else {
            Write-Host "${a}: DIFFERENT" -ForegroundColor Yellow
            $bad = 1
        }
    }
    if ($bad) { Write-Host 'M4 COMPARE: DIFFERENT' -ForegroundColor Yellow } else { Write-Host 'M4 COMPARE: SAME TEXT' -ForegroundColor Green }
}
Compress-Archive -Force -Path $Out -DestinationPath "$Out.zip"
Write-Host "win-run: done, send $Out.zip"
