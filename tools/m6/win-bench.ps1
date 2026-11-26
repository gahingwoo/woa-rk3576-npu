# win-bench.ps1 - M6 on the board: the driver tests, then charsiu arms, then one table
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File win-bench.ps1 -Model R:\npu\Llama-3.2-1B-Instruct-Q8_0.gguf [-Out R:\npu\m6-windows] [-Linux R:\npu\m4-linux] [-NoFault] [-Quick] [-IdleDisable]
#
# As administrator, from the unpacked charsiu-m6-arm64 artifact with
# rknputest.exe from rknpu-m6-arm64 beside it (3686706:docs/history/2026-11-24-m6.md). Each step writes
# its own files under -Out, so a run that dies part way still says how far
# it got; at the end bench.txt holds the table and bench.csv the numbers.
#
# Steps:
#   1. rknputest info (must say m6, IOMMU), m3 (the ladder through the page
#      table), m6 (scatter 768 MiB, latency, both cores at once and, unless
#      -NoFault, a deliberate fault into a guard page and the recovery)
#   2. charsiu arms, the M4 ones with the same arguments plus:
#        short-npu1   CHARSIU_NPU_ONEDEV=1, one core: the both-cores control
#        short-wc     needs Device Parameters\M6CachedBos = 0 and a reboot;
#                     not run here, see 3686706:docs/history/2026-11-24-m6.md
#        stages       CHARSIU_STAGES=1, 32 tokens: where a token goes
#   3. bench.txt: per arm load, staging, prompt and decode tok/s, NPU busy
#      (both devices' hw time), the NPU's share of the wall clock, submits,
#      timeouts / faults, page faults and peak working set, and whether the
#      NPU arm repeats itself byte for byte.
#
# -Quick runs only short-cpu, short-npu, short-npu2 and short-npu1.
# -IdleDisable sets the processor idle-disable power setting for the arms and
# restores it afterwards: the Windows counterpart of the /dev/cpu_dma_latency
# hold charsiu takes on Linux ("costs about a quarter of decode" without it).
#
# Every program runs through cmd.exe so its stdout reaches the file as bytes.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Model,
    [string]$Out = '.\m6-windows',
    [string]$Linux = '',
    [switch]$NoFault,
    [switch]$Quick,
    [switch]$IdleDisable
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $here 'charsiu_run.exe'
$long = Join-Path $here 'long.txt'
$sha = '3f87a880027e7b9ea8e0da9e4009584336f352af444a0e6e5c20721ac4c7ffd1'
$prompt = 'Write a short story about a lighthouse keeper who finds a message in a bottle.'
$crumbs = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0\Device Parameters'

function Die($m) { Write-Host "win-bench: $m" -ForegroundColor Red; exit 1 }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die 'run as administrator (\\.\RKNPU is administrators-only)'
}
foreach ($f in $run, $long) { if (-not (Test-Path $f)) { Die "missing ${f}: run from the unpacked artifact" } }
$rkt = Join-Path $here 'rknputest.exe'
if (-not (Test-Path $rkt)) { $rkt = (Get-Command rknputest.exe -ErrorAction SilentlyContinue).Source }
if (-not $rkt) { Die 'rknputest.exe not found (copy it from rknpu-m6-arm64 beside this script)' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

Get-ChildItem Env: | Where-Object { $_.Name -like 'CHARSIU_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }

Write-Host 'win-bench: checking the model (1.3 GB)'
$got = (Get-FileHash -Algorithm SHA256 $Model).Hash.ToLower()
if ($got -ne $sha) { Die "$Model is not Llama-3.2-1B-Instruct-Q8_0.gguf (sha256 $got)" }

$bat = Join-Path $Out 'run.cmd'
function Cmd($line, $stdout, $stderr) {
    $body = "@echo off`r`n$line > `"$stdout`" 2> `"$stderr`"`r`nexit /b %errorlevel%`r`n"
    [IO.File]::WriteAllText($bat, $body, [Text.Encoding]::ASCII)
    & cmd.exe /d /c "`"$bat`"" | Out-Host
    return $LASTEXITCODE
}

# stdout without the lines that start with '[', as grep -v '^\[' writes it
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

& {
    Get-CimInstance Win32_OperatingSystem | Format-List Caption, Version, BuildNumber, TotalVisibleMemorySize, FreePhysicalMemory
    Get-Date -Format o
    Get-FileHash -Algorithm SHA256 $run, $rkt | Format-Table -AutoSize
    pnputil /enum-devices /instanceid 'ACPI\RKCP3576\0' /drivers
    reg query $crumbs
} *>&1 | Out-File -Encoding ascii (Join-Path $Out 'system.txt')
Cmd "`"$run`" --version" (Join-Path $Out 'version.txt') (Join-Path $Out 'version.err') | Out-Null

# ---- 1. the driver -----------------------------------------------------------

Write-Host 'win-bench: rknputest info'
Cmd "`"$rkt`" info" (Join-Path $Out 'info-before.txt') (Join-Path $Out 'info-before.err') | Out-Null
if (-not (Select-String -Quiet -Path (Join-Path $Out 'info-before.txt') -Pattern '^m6: version .*memory IOMMU')) {
    Die "the driver is not the M6 build in IOMMU mode (see $Out\info-before.txt)"
}
Write-Host 'win-bench: rknputest m3'
$rc = Cmd "`"$rkt`" m3" (Join-Path $Out 'm3.txt') (Join-Path $Out 'm3.err')
if ($rc -ne 0) { Die "rknputest m3 failed ($rc): stop here and send $Out" }
$m6args = if ($NoFault) { 'm6' } else { 'm6 --fault' }
Write-Host "win-bench: rknputest $m6args"
$rc = Cmd "`"$rkt`" $m6args" (Join-Path $Out 'm6.txt') (Join-Path $Out 'm6.err')
if ($rc -ne 0) { Die "rknputest m6 failed ($rc): stop here and send $Out" }

# ---- 2. charsiu ----------------------------------------------------------------

$npu = @{ CHARSIU_NPU = '1'; CHARSIU_NPU_QUANT = '1'; CHARSIU_NPU_W4V = '1'; CHARSIU_NPU_KMAX = '1024';
          CHARSIU_NPU_W4_GROUP = '1024'; CHARSIU_NPU_MAXN = '262144'; CHARSIU_COEF_ELEMS = '65536';
          CHARSIU_RKNPU_STATS = '1' }

$idleOld = $null
if ($IdleDisable) {
    $q = powercfg /query SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE | Select-String 'Current AC Power Setting Index: (0x[0-9a-f]+)'
    if ($q) { $idleOld = $q.Matches[0].Groups[1].Value }
    powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE 1 | Out-Null
    powercfg /setactive SCHEME_CURRENT | Out-Null
    Write-Host "win-bench: processor idle disabled for the arms (was $idleOld)"
}

$arms = New-Object System.Collections.ArrayList
function Arm($name, $useNpu, $argline, $extra) {
    Write-Host "win-bench: ${name}"
    $set = @{}
    if ($useNpu) { foreach ($k in $npu.Keys) { $set[$k] = $npu[$k] } }
    if ($extra) { foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] } }
    foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
    $t0 = Get-Date
    $rc = Cmd "`"$run`" `"$Model`" $argline -t 4" (Join-Path $Out "$name.txt") (Join-Path $Out "$name.err")
    $s = [int]((Get-Date) - $t0).TotalSeconds
    foreach ($k in $set.Keys) { Remove-Item "Env:$k" }
    if ($rc -ne 0) { Add-Content (Join-Path $Out "$name.err") "exit $rc" }
    Write-Text (Join-Path $Out "$name.txt") (Join-Path $Out "$name.text")
    Write-Host "  exit $rc, $s s"
    reg query $crumbs | Out-File -Encoding ascii (Join-Path $Out "$name.crumbs.txt")
    [void]$arms.Add(@{ Name = $name; Npu = $useNpu; Rc = $rc; Wall = $s })
}

$p = "-p `"$prompt`""
try {
    Arm 'short-cpu' $false "$p -n 128 -c 512" $null
    Arm 'short-npu' $true "$p -n 128 -c 512" $null
    Arm 'short-npu2' $true "$p -n 128 -c 512" $null
    Arm 'short-npu1' $true "$p -n 128 -c 512" @{ CHARSIU_NPU_ONEDEV = '1' }
    if (-not $Quick) {
        Arm 'stages' $true "$p -n 32 -c 512" @{ CHARSIU_STAGES = '1' }
        Arm 'long-cpu' $false "-f `"$long`" -n 64 -c 1024" $null
        Arm 'long-npu' $true "-f `"$long`" -n 64 -c 1024" $null
        Arm 'logits-cpu' $false "$p -c 512 --logits 10" $null
        Arm 'logits-npu' $true "$p -c 512 --logits 10" $null
    }
} finally {
    if ($IdleDisable -and $idleOld) {
        powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE $idleOld | Out-Null
        powercfg /setactive SCHEME_CURRENT | Out-Null
        Write-Host "win-bench: processor idle setting restored ($idleOld)"
    }
}

Cmd "`"$rkt`" info" (Join-Path $Out 'info-after.txt') (Join-Path $Out 'info-after.err') | Out-Null

# ---- 3. the table --------------------------------------------------------------

function Num($m, $i) { if ($m) { [double]$m.Groups[$i].Value } else { $null } }

$rows = foreach ($a in $arms) {
    $txt = Get-Content -Raw (Join-Path $Out "$($a.Name).txt") -ErrorAction SilentlyContinue
    $err = Get-Content (Join-Path $Out "$($a.Name).err") -ErrorAction SilentlyContinue
    if (-not $txt) { $txt = '' }
    $mLoad = [regex]::Match($txt, '\[load (\d+) ms')
    $mStage = [regex]::Match($txt, 'staging (\d+) ms')
    $mPrompt = [regex]::Match($txt, 'prompt (\d+) tok in (\d+) ms, ([0-9.]+) tok/s')
    $mGen = [regex]::Match($txt, 'gen (\d+) tok in (\d+) ms, ([0-9.]+) tok/s')
    $hw = 0.0; $waited = 0.0; $submits = 0; $bad = 0; $devs = 0; $pf = ''; $ws = ''; $preps = 0
    foreach ($l in $err) {
        $d = [regex]::Match($l, '^charsiu rknpu dev \d+: (\d+) submits.*, (\d+) timeouts, (\d+) dma errors, (\d+) refused;.*NPU ([0-9.]+) ms, waited ([0-9.]+) ms(; .*?, .*?, (\d+) preps \+ (\d+) finis)?')
        if ($d.Success) {
            $devs++
            $submits += [int]$d.Groups[1].Value
            $bad += [int]$d.Groups[2].Value + [int]$d.Groups[3].Value + [int]$d.Groups[4].Value
            $hw += [double]$d.Groups[5].Value
            $waited += [double]$d.Groups[6].Value
            if ($d.Groups[8].Success) { $preps += [int]$d.Groups[8].Value + [int]$d.Groups[9].Value }
        }
        $m = [regex]::Match($l, '(\d+) page faults, peak working set (\d+) MiB')
        if ($m.Success) { $pf = $m.Groups[1].Value; $ws = $m.Groups[2].Value }
    }
    $fallback = @($err | Where-Object { $_ -match 'NOT on the NPU|rknpu buffer of' }).Count
    $runMs = 0
    if ($mPrompt.Success) { $runMs += [double]$mPrompt.Groups[2].Value }
    if ($mGen.Success) { $runMs += [double]$mGen.Groups[2].Value }
    $share = if ($a.Npu -and $runMs -gt 0 -and $devs -gt 0) { [math]::Round(100.0 * $hw / ($devs * $runMs), 1) } else { $null }
    [pscustomobject]@{
        arm = $a.Name; exit = $a.Rc; wall_s = $a.Wall
        load_ms = Num $mLoad 1; staging_ms = Num $mStage 1
        prompt_tok_s = Num $mPrompt 3; gen_tok_s = Num $mGen 3
        npu_devices = $devs; npu_busy_ms = [math]::Round($hw, 1); npu_waited_ms = [math]::Round($waited, 1)
        npu_share_pct = $share; submits = $submits; syncs = $preps; errors = $bad; fallbacks = $fallback
        page_faults = $pf; peak_ws_mib = $ws
    }
}
$rows | Export-Csv -NoTypeInformation -Path (Join-Path $Out 'bench.csv')

$lines = Get-ChildItem (Join-Path $Out '*.text') | Sort-Object Name | ForEach-Object {
    '{0}  ./{1}' -f (Get-FileHash -Algorithm MD5 $_.FullName).Hash.ToLower(), $_.Name
}
[IO.File]::WriteAllText((Join-Path $Out 'text.md5'), (($lines -join "`n") + "`n"))
$same = (Get-FileHash -Algorithm MD5 (Join-Path $Out 'short-npu.text')).Hash -eq (Get-FileHash -Algorithm MD5 (Join-Path $Out 'short-npu2.text')).Hash

$m6 = Get-Content (Join-Path $Out 'm6.txt')
$sum = New-Object System.Collections.ArrayList
[void]$sum.Add('M6 bench, ' + (Get-Date -Format o))
foreach ($k in 'SCATTER', 'PARALLEL:', 'LATENCY:', 'IOMMU FAULT as intended', 'RKNPU M6:') {
    $hit = $m6 | Where-Object { $_ -match [regex]::Escape($k) } | Select-Object -First 1
    if ($hit) { [void]$sum.Add('  ' + $hit.Trim()) }
}
[void]$sum.Add(($rows | Format-Table -AutoSize arm, exit, staging_ms, prompt_tok_s, gen_tok_s, npu_devices, npu_busy_ms, npu_share_pct, submits, errors, fallbacks, page_faults, peak_ws_mib | Out-String -Width 220))
[void]$sum.Add($(if ($same) { 'short-npu and short-npu2: SAME TEXT (the NPU arm repeats itself)' } else { 'short-npu and short-npu2: DIFFERENT TEXT' }))
$cpu = $rows | Where-Object { $_.arm -eq 'short-cpu' }
$n2 = $rows | Where-Object { $_.arm -eq 'short-npu' }
$n1 = $rows | Where-Object { $_.arm -eq 'short-npu1' }
if ($cpu.gen_tok_s -and $n2.gen_tok_s) { [void]$sum.Add(('decode: NPU {0} tok/s vs CPU {1} tok/s = {2:N2}x' -f $n2.gen_tok_s, $cpu.gen_tok_s, ($n2.gen_tok_s / $cpu.gen_tok_s))) }
if ($n1.gen_tok_s -and $n2.gen_tok_s) { [void]$sum.Add(('decode: both cores {0} tok/s vs one core {1} tok/s = {2:N2}x' -f $n2.gen_tok_s, $n1.gen_tok_s, ($n2.gen_tok_s / $n1.gen_tok_s))) }
[void]$sum.Add('M4 on this board (2026-11-24): short-cpu 3.72 / 2.88 tok/s, short-npu 1.26 / 0.85 tok/s (prompt / decode), staging 77.9 s, 29 buffer failures')
[void]$sum.Add('Linux, charsiu on rocket (README, different model): TinyLlama 1.1B 21.88 tok/s decode; Llama-3.2-1B: run linux-ref.sh and pass -Linux')
if ($Linux) {
    foreach ($a in 'short-cpu', 'short-npu', 'long-cpu', 'long-npu', 'logits-cpu', 'logits-npu') {
        $l = Join-Path $Linux "$a.text"
        $w = Join-Path $Out "$a.text"
        if ((Test-Path $l) -and (Test-Path $w)) {
            $r = if ((Get-FileHash -Algorithm MD5 $l).Hash -eq (Get-FileHash -Algorithm MD5 $w).Hash) { 'SAME' } else { 'DIFFERENT' }
            [void]$sum.Add("linux ${a}: $r")
        }
        $lt = Join-Path $Linux "$a.txt"
        if (Test-Path $lt) {
            $g = [regex]::Match((Get-Content -Raw $lt), 'gen (\d+) tok in (\d+) ms, ([0-9.]+) tok/s')
            if ($g.Success) { [void]$sum.Add("linux ${a}: decode $($g.Groups[3].Value) tok/s") }
        }
    }
}
[IO.File]::WriteAllText((Join-Path $Out 'bench.txt'), (($sum -join "`r`n") + "`r`n"))
$sum | ForEach-Object { Write-Host $_ }
Compress-Archive -Force -Path $Out -DestinationPath "$Out.zip"
Write-Host "win-bench: done, send $Out.zip"
